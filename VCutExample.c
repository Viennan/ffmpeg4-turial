/*
 * Cut video file with minimum decoding/encoding
 * Author: Wang Yingnan
 * Github: Viennan
 * Email: wangynine@163.com
 * Complete at 2022.05.03
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#define ERR_CACHE_EMPTY -30001
#define ERR_REACH_ENDPTS -30002
#define ERR_TRANS_MODE_CHANGE -30003

typedef struct PacketNode {
    AVPacket* packet;
    struct PacketNode* next;
} PacketNode;

static PacketNode* packet_node_alloc(AVPacket* packet) {
    PacketNode* p = av_mallocz(sizeof(PacketNode));
    p->packet = packet ? av_packet_clone(packet) : NULL;
    return p;
}

static void packet_node_free(PacketNode* p) {
    if (p) {
        av_packet_free(&p->packet);
        av_free(p);
    }
}

typedef struct PacketList {
    PacketNode *head, *tail;
    int size;
} PacketList;

static void init_packet_list(PacketList* p) {
    p->size = 0;
    p->head = packet_node_alloc(NULL);
    p->tail = p->head;
}

static void push_packet(PacketList* p, AVPacket* data) {
    p->size++;
    p->tail->next = packet_node_alloc(data);
    p->tail = p->tail->next;
}

static AVPacket* peek_packet(PacketList* p) {
    return p->size > 0 ? p->head->next->packet : NULL;
}

static void push_front_packet(PacketList* p, AVPacket* data) {
    PacketNode* node = packet_node_alloc(data);
    node->next = p->head->next;
    if (p->size == 0) {
        p->tail = node;
    }
    p->head->next = node;
    p->size++;
}

static void pop_packet(PacketList* p, AVPacket* data) {
    av_packet_unref(data);
    if (p->tail == p->head) {
        return;
    }
    PacketNode* node = p->head;
    p->head = node->next;
    packet_node_free(node);
    AVPacket* packet = p->head->packet;
    p->head->packet = NULL;
    av_packet_ref(data, packet);
    av_packet_free(&packet);
    p->size--;
    return;
}

static void packet_list_clear(PacketList* p) {
    if (!p) {
        return;
    }
    while (p->head != p->tail) {
        PacketNode* node = p->tail;
        p->tail = node->next;
        packet_node_free(node);
    }
    p->size = 0;
}

// Release the packet list.
// Must call init_packet_list before reuse.
static void packet_list_release(PacketList* p) {
    if (!p) {
        return;
    }
    while (p->head != p->tail) {
        PacketNode* node = p->head;
        p->head = node->next;
        packet_node_free(node);
    }
    if (p->tail) {
        packet_node_free(p->tail);
        p->tail = NULL;
        p->head = NULL;
    }
    p->size = 0;
}

// When in copy mode, a packet will be push-in and pop-out in one function
// call.(the most common case) The followed struct is optimized for avoiding
// frequently AVPacket alloction under single packet cache situation.
typedef struct PacketCache {
    int size;
    AVPacket* header;
    PacketList list;
} Cache;

static inline void initCache(Cache* c) {
    c->size = 0;
    c->header = av_packet_alloc();
    init_packet_list(&c->list);
}

static inline void closeCache(Cache* c) {
    c->size = 0;
    av_packet_free(&c->header);
    packet_list_release(&c->list);
}

static inline void clearCache(Cache* c) {
    c->size = 0;
    av_packet_unref(c->header);
    packet_list_clear(&c->list);
}

static inline void push(Cache* c, AVPacket* p) {
    if (c->size == 0) {
        av_packet_ref(c->header, p);
    } else {
        push_packet(&c->list, p);
    }
    c->size++;
}

static inline void push_front(Cache* c, AVPacket* p) {
    if (c->size > 0) {
        push_front_packet(&c->list, c->header);
        av_packet_unref(c->header);
    }
    av_packet_ref(c->header, p);
    c->size++;
}

static inline void pop(Cache* c, AVPacket* p) {
    av_packet_unref(p);
    if (c->size == 0) {
        return;
    }
    av_packet_ref(p, c->header);
    pop_packet(&c->list, c->header);
    c->size--;
}

enum TranscodeMode { Copy, TransCode };

typedef struct StreamContext {
    int stream_id;
    enum AVMediaType stream_type;

    int64_t startPts, endPts;
    int64_t encodeStPts, encodePtsOffset;
    int64_t encodeDtsOffset;
    int64_t copyStPts, copyEndPts;
    int64_t copyStDts, copyDtsOffset;

    int64_t copyDtsDelta;

    AVCodecContext *pEncodeCtx, *pDecodeCtx;
    Cache i_cache, o_cache;
    AVFrame* frameBuf;
    AVPacket* packetBuf;
    AVStream *i_stream, *o_stream;
    enum TranscodeMode transMode;
    int error_code;

    AVBSFContext *encode_out_bsf, *copy_out_bsf;
    AVPacket* bsf_buf;
} StreamContext;

static inline int64_t streamStdScale(StreamContext* ctx, int64_t ts,
                                     AVRational tb) {
    return av_rescale_q_rnd(ts, tb, ctx->i_stream->time_base,
                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
}

static inline int64_t streamStdRescale(StreamContext* ctx, int64_t ts,
                                       AVRational tb) {
    return av_rescale_q_rnd(ts, ctx->i_stream->time_base, tb,
                            AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
}

// Param p should not be NULL
static inline void streamScaleOutPacket(StreamContext* ctx, AVPacket* p) {
    p->pts = av_rescale_q_rnd(p->pts, ctx->i_stream->time_base,
                              ctx->o_stream->time_base,
                              AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
    p->dts = av_rescale_q_rnd(p->dts, ctx->i_stream->time_base,
                              ctx->o_stream->time_base,
                              AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
}

static void streamFreeBsfs(StreamContext* ctx) {
    if (ctx->encode_out_bsf) {
        av_bsf_free(&ctx->encode_out_bsf);
    }
    if (ctx->copy_out_bsf) {
        av_bsf_free(&ctx->copy_out_bsf);
    }
}

static int open_h264bsfcs(StreamContext* ctx) {
    int ret;
    streamFreeBsfs(ctx);
    const AVBitStreamFilter* h264bsf = av_bsf_get_by_name("h264_mp4toannexb");
    av_bsf_alloc(h264bsf, &ctx->encode_out_bsf);
    av_bsf_alloc(h264bsf, &ctx->copy_out_bsf);
    ctx->encode_out_bsf->par_in = avcodec_parameters_alloc();
    ctx->copy_out_bsf->par_in = avcodec_parameters_alloc();
    avcodec_parameters_from_context(ctx->encode_out_bsf->par_in,
                                    ctx->pEncodeCtx);
    avcodec_parameters_from_context(ctx->copy_out_bsf->par_in, ctx->pDecodeCtx);
    if ((ret = av_bsf_init(ctx->encode_out_bsf)) < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Failed init h264_mp4toannexb of trancode mode: %s\n",
               av_err2str(ret));
        goto end_open_h264bsfcs;
    }
    if ((ret = av_bsf_init(ctx->copy_out_bsf)) < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Failed init h264_mp4toannexb of copy mode: %s\n",
               av_err2str(ret));
        goto end_open_h264bsfcs;
    }

end_open_h264bsfcs:
    return ret;
}

static void filterPacket(StreamContext* ctx, Cache* buf, AVBSFContext* bsfc,
                         AVPacket* p) {
    int ret = 0;
    av_packet_unref(ctx->bsf_buf);
    av_packet_ref(ctx->bsf_buf, p);
    if ((ret = av_bsf_send_packet(bsfc, ctx->bsf_buf)) < 0) {
        push(buf, p);
        return;
    }
    av_packet_unref(ctx->bsf_buf);
    int loop_in = 0;
    while ((ret = av_bsf_receive_packet(bsfc, ctx->bsf_buf)) >= 0) {
        loop_in = 1;
        push(buf, ctx->bsf_buf);
        av_packet_unref(ctx->bsf_buf);
    }
    if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
        if (!loop_in) {
            push(buf, p);
            return;
        }
    }
}

static int open_codec_context(AVStream* stream, AVCodecContext** ps,
                              int encode_flag) {
    int ret;
    AVDictionary* options = NULL;
    AVCodecParameters* codecpar = avcodec_parameters_alloc();
    avcodec_parameters_copy(codecpar, stream->codecpar);
    AVCodec* codec = encode_flag ? avcodec_find_encoder(codecpar->codec_id)
                                 : avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        av_log(NULL, AV_LOG_ERROR, "Failed to find %s codec\n",
               av_get_media_type_string(codecpar->codec_type));
        ret = AVERROR(EINVAL);
        goto end_open_codec;
    }

    /* Allocate a codec context for the decoder */
    *ps = avcodec_alloc_context3(codec);
    if (!*ps) {
        av_log(NULL, AV_LOG_ERROR, "Failed to allocate the %s codec context\n",
               av_get_media_type_string(codecpar->codec_type));
        ret = AVERROR(ENOMEM);
        goto end_open_codec;
    }

    /* Copy codec parameters from input stream to output codec context */
    if ((ret = avcodec_parameters_to_context(*ps, codecpar)) < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Failed to copy %s codec parameters to decoder context\n",
               av_get_media_type_string(codecpar->codec_type));
        goto end_open_codec;
    }

    if (encode_flag) {
        AVCodecContext* p = *ps;
        p->time_base = stream->time_base;
        // Fix me: how to determine the following params from input stream
        av_dict_set(&options, "crf", "18", 0);
    }

    /* Init the codec */
    if ((ret = avcodec_open2(*ps, codec, &options)) < 0) {
        fprintf(stderr, "Failed to open %s codec\n",
                av_get_media_type_string(codecpar->codec_type));
        goto end_open_codec;
    }

end_open_codec:
    avcodec_parameters_free(&codecpar);
    if (options) {
        av_dict_free(&options);
    }
    return ret;
}

static int initFromAVStream(StreamContext* ctx, AVStream* i_stream,
                            AVStream* o_stream) {
    ctx->copyStPts = AV_NOPTS_VALUE;
    ctx->copyEndPts = AV_NOPTS_VALUE;
    ctx->copyStDts = AV_NOPTS_VALUE;
    ctx->copyDtsDelta = 0;
    ctx->stream_id = o_stream->index;
    ctx->stream_type = i_stream->codecpar->codec_type;
    initCache(&ctx->i_cache);
    initCache(&ctx->o_cache);
    ctx->frameBuf = av_frame_alloc();
    ctx->packetBuf = av_packet_alloc();
    ctx->bsf_buf = av_packet_alloc();
    ctx->error_code = 0;
    ctx->o_stream = o_stream;
    ctx->i_stream = i_stream;
    ctx->encode_out_bsf = NULL;
    ctx->copy_out_bsf = NULL;
    int ret;
    if ((ret = open_codec_context(i_stream, &ctx->pDecodeCtx, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed open decoding context");
        return ret;
    }
    if ((ret = open_codec_context(i_stream, &ctx->pEncodeCtx, 1)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed open encoding context");
        return ret;
    }
    if (o_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
        if ((ret = open_h264bsfcs(ctx)) < 0) {
            return ret;
        }
    }
    return 0;
}

static void closeStreamContext(StreamContext* ctx) {
    if (!ctx) {
        return;
    }
    closeCache(&ctx->i_cache);
    closeCache(&ctx->o_cache);
    av_frame_free(&ctx->frameBuf);
    av_packet_free(&ctx->packetBuf);
    av_packet_free(&ctx->bsf_buf);
    avcodec_free_context(&ctx->pEncodeCtx);
    avcodec_free_context(&ctx->pDecodeCtx);
    streamFreeBsfs(ctx);
}

static void relocatePts(StreamContext* ctx) {
    if (ctx->copyStPts >= ctx->copyEndPts || ctx->copyStPts < ctx->startPts) {
        ctx->copyStPts = LONG_MAX;
        ctx->copyEndPts = LONG_MAX;
        ctx->transMode = TransCode;
    }
}

static inline int getOutputPacketFromCache(StreamContext* ctx,
                                           AVPacket* p_out) {
    int ret = AVERROR(EAGAIN);
    if (ctx->o_cache.size > 0) {
        pop(&ctx->o_cache, p_out);
        ret = 0;
    }
    return ret;
}

typedef int (*StreamTranscodeFunc)(StreamContext* ctx, AVPacket* p,
                                   AVPacket* p_out);
typedef int (*StreamFlushFunc)(StreamContext* ctx, AVPacket* p_out);

static int audioStreamSeekStart(StreamContext* ctx, AVPacket* p) {
    if (p->pts + p->duration >= ctx->startPts) {
        push(&ctx->o_cache, p);
        if (p->pts <= ctx->startPts) {
            ctx->startPts = p->pts;
            ctx->copyStDts = p->dts;
            ctx->transMode = Copy;
        }
        return 1;
    }
    return 0;
}

static int videoStreamSeekStart(StreamContext* ctx, AVPacket* p) {
    push(&ctx->i_cache, p);
    if (ctx->i_cache.size == 1) {
        if (p->pts == AV_NOPTS_VALUE) {
            av_log(NULL, AV_LOG_ERROR, "Do not support this file type.");
            return AVERROR(EINVAL);
        }
        ctx->transMode = TransCode;
        ctx->encodeStPts = ctx->startPts;
        ctx->encodePtsOffset = 0;
        ctx->encodeDtsOffset = 0;
        if (p->pts + p->duration >= ctx->startPts &&
            ctx->copyStPts <= ctx->endPts) {
            ctx->transMode = Copy;
            ctx->copyStDts = p->dts;
            ctx->copyDtsOffset = 0;
        }
    }
    return 1;
}

static int _retrieve_packet(Cache* cache, AVPacket* p, AVPacket* p_out) {
    if (p) {
        push(cache, p);
    }
    if (cache->size == 0) {
        return AVERROR(EAGAIN);
    }
    pop(cache, p_out);
    return 0;
}

static int audioTranscode(StreamContext* ctx, AVPacket* p, AVPacket* p_out) {
    int ret;
    if (ctx->error_code == ERR_REACH_ENDPTS) {
        p = NULL;
    }
    if ((ret = _retrieve_packet(&ctx->o_cache, p, p_out)) < 0) {
        return ctx->error_code == ERR_REACH_ENDPTS ? AVERROR_EOF
                                                   : AVERROR(EAGAIN);
    }
    if (p_out->pts > ctx->endPts) {
        ctx->error_code = ERR_REACH_ENDPTS;
        clearCache(&ctx->o_cache);
        return AVERROR_EOF;
    }
    p_out->pts -= ctx->startPts;
    p_out->dts -= ctx->copyStDts;
    p_out->stream_index = ctx->stream_id;
    streamScaleOutPacket(ctx, p_out);
    return 0;
}

static int audioFlush(StreamContext* ctx, AVPacket* p_out) {
    ctx->error_code = ERR_REACH_ENDPTS;
    int ret = getOutputPacketFromCache(ctx, p_out);
    if (ret == AVERROR(EAGAIN)) {
        ret = AVERROR_EOF;
    }
    p_out->pts -= ctx->startPts;
    p_out->dts -= ctx->copyStDts;
    p_out->stream_index = ctx->stream_id;
    streamScaleOutPacket(ctx, p_out);
    return ret;
}

static inline int64_t genCopyDts(StreamContext* ctx, int64_t in_dts) {
    int64_t diff_dts = in_dts - ctx->copyStDts;
    if (diff_dts < 0 || diff_dts < ctx->copyDtsDelta) {
        diff_dts = ctx->copyDtsDelta;
    }
    ctx->copyDtsDelta = diff_dts + 2;
    return diff_dts + ctx->copyDtsOffset;
}

static int copyMode(StreamContext* ctx, AVPacket* i_packet, AVPacket* p_out) {
    int ret = 0;
    if (ctx->error_code == ERR_REACH_ENDPTS) {
        i_packet = NULL;
    }
    if ((ret = getOutputPacketFromCache(ctx, p_out)) >= 0) {
        if (i_packet)
            push(&ctx->i_cache, i_packet);
        return ret;
    }
    ret = _retrieve_packet(&ctx->i_cache, i_packet, p_out);
    if (ret < 0) {
        if (ctx->error_code == ERR_REACH_ENDPTS) {
            return AVERROR_EOF;
        }
        return ret;
    }
    if (p_out->pts != AV_NOPTS_VALUE && p_out->pts >= ctx->copyEndPts) {
        if (p_out->pts > ctx->endPts) {
            ctx->error_code = ERR_REACH_ENDPTS;
            ret = AVERROR_EOF;
        } else {
            ctx->transMode = TransCode;
            ctx->encodeStPts = p_out->pts;
            ctx->encodePtsOffset = p_out->pts - ctx->startPts;
            ctx->encodeDtsOffset = ctx->encodePtsOffset;
            avcodec_flush_buffers(ctx->pDecodeCtx);

            // reopen encoder
            avcodec_free_context(&ctx->pEncodeCtx);
            int e_ret;
            if ((e_ret = open_codec_context(ctx->i_stream, &ctx->pEncodeCtx,
                                            1)) < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Failed reopen encoding context: %s\n", av_err2str(ret));
                return AVERROR_EXIT;
            }

            push_front(&ctx->i_cache, p_out);
            av_packet_unref(p_out);
            ret = ERR_TRANS_MODE_CHANGE;
        }
        return ret;
    }
    p_out->pts -= ctx->startPts;
    p_out->dts = genCopyDts(ctx, p_out->dts);
    p_out->stream_index = ctx->stream_id;
    streamScaleOutPacket(ctx, p_out);
    if (ctx->copy_out_bsf) {
        filterPacket(ctx, &ctx->o_cache, ctx->copy_out_bsf, p_out);
        ret = getOutputPacketFromCache(ctx, p_out);
    }
    return ret;
}

static void encode(StreamContext* ctx, AVFrame* pFrame) {
    if (pFrame) {
        pFrame->pict_type = AV_PICTURE_TYPE_NONE;
        pFrame->pts -= ctx->encodeStPts;
        pFrame->pts = FFMAX(0, pFrame->pts);
    }
    int ret = avcodec_send_frame(ctx->pEncodeCtx, pFrame);
    if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
        av_log(NULL, AV_LOG_ERROR, "Failed send frame to encoder: %s\n",
               av_err2str(ret));
    }
    AVPacket* p = ctx->packetBuf;
    while (avcodec_receive_packet(ctx->pEncodeCtx, p) >= 0) {
        int64_t pts = p->pts;
        p->dts += ctx->encodeDtsOffset;
        // Offset dts a little bit if the next iteration turning into copy mode
        ctx->copyDtsOffset = p->dts + 2;
        p->pts += ctx->encodePtsOffset;
        p->stream_index = ctx->stream_id;
        streamScaleOutPacket(ctx, p);
        if (pts == 0 && ctx->encode_out_bsf) {
            filterPacket(ctx, &ctx->o_cache, ctx->encode_out_bsf, p);
        } else {
            push(&ctx->o_cache, p);
        }
    }
}

static int retrieveAndSend(StreamContext* ctx) {
    AVFrame* pFrame = ctx->frameBuf;
    int d_ret;
    while ((d_ret = avcodec_receive_frame(ctx->pDecodeCtx, pFrame)) >= 0) {
        if (pFrame->pts < ctx->startPts) {
            continue;
        }
        if (pFrame->pts > ctx->endPts) {
            encode(ctx, NULL);
            ctx->error_code = ERR_REACH_ENDPTS;
            return ERR_REACH_ENDPTS;
        }
        encode(ctx, pFrame);
    }
    if (d_ret < 0 && d_ret != AVERROR_EOF && d_ret != AVERROR(EAGAIN)) {
        av_log(NULL, AV_LOG_ERROR, "Failed decode video: %s\n",
               av_err2str(d_ret));
    }
    return AVERROR(EAGAIN);
}

static int transcode(StreamContext* ctx, AVPacket* p) {
    int ret = avcodec_send_packet(ctx->pDecodeCtx, p);
    int encode_ret = retrieveAndSend(ctx);
    if (ret == AVERROR(EAGAIN) && encode_ret != ERR_REACH_ENDPTS) {
        ret = avcodec_send_packet(ctx->pDecodeCtx, p);
        encode_ret = retrieveAndSend(ctx);
    }
    if (!p) {
        encode(ctx, NULL);
    }
    return encode_ret == ERR_REACH_ENDPTS ? ERR_REACH_ENDPTS : 0;
}

static int transcodeMode(StreamContext* ctx, AVPacket* i_packet,
                         AVPacket* p_out) {
    int ret;
    ret = getOutputPacketFromCache(ctx, p_out);
    if (ret >= 0) {
        goto exitTranscodeMode;
    }
    if (ret < 0 && ctx->error_code == ERR_REACH_ENDPTS) {
        ret = AVERROR_EOF;
        goto exitTranscodeMode;
    }
    ret = _retrieve_packet(&ctx->i_cache, i_packet, p_out);
    while (ret >= 0) {
        if (p_out->pts != AV_NOPTS_VALUE && p_out->pts >= ctx->copyStPts &&
            p_out->pts < ctx->copyEndPts) {
            // Flush decoder / encoder
            transcode(ctx, NULL);

            push_front(&ctx->i_cache, p_out);
            // Use pts of the packet to relocate dts
            ctx->copyStDts = p_out->pts;
            av_packet_unref(p_out);
            ctx->transMode = Copy;
            ret = ERR_TRANS_MODE_CHANGE;
            goto exitTranscodeMode;
        }
        transcode(ctx, p_out);
        ret = _retrieve_packet(&ctx->i_cache, NULL, p_out);
    }
    ret = getOutputPacketFromCache(ctx, p_out);

exitTranscodeMode:
    return ret;
}

static int videoTranscode(StreamContext* ctx, AVPacket* p, AVPacket* p_out) {
    int ret = 0;
    switch (ctx->transMode) {
    case Copy:
        ret = copyMode(ctx, p, p_out);
        break;
    case TransCode:
        ret = transcodeMode(ctx, p, p_out);
        break;
    default:
        av_log(NULL, AV_LOG_ERROR, "Unknown Transcode Mode\n");
        return AVERROR_EXIT;
        break;
    }
    return ret;
}

static int videoFlush(StreamContext* ctx, AVPacket* p_out) {
    int ret = AVERROR_EOF;
    // When encounter ERR_TRANS_MODE_CHANGE, recall the function
    while ((ret = videoTranscode(ctx, NULL, p_out)) == ERR_TRANS_MODE_CHANGE)
        ;
    if (ret == AVERROR(EAGAIN)) {
        if (ctx->error_code != ERR_REACH_ENDPTS &&
            ctx->transMode == TransCode) {
            transcode(ctx, NULL);
            ret = videoTranscode(ctx, NULL, p_out);
        }
    }
    if (ret < 0 && ret != AVERROR_EXIT) {
        ctx->error_code = ERR_REACH_ENDPTS;
        ret = AVERROR_EOF;
    }
    return ret;
}

static void seek_first_pts(AVFormatContext* ifmt, int* stream_mapping,
                           int valid_stream_num, int64_t* buf) {
    AVPacket* p = av_packet_alloc();
    for (int i = 0; i < valid_stream_num; ++i) {
        buf[i] = AV_NOPTS_VALUE;
    }
    int ready_num = 0;
    while (ready_num < valid_stream_num && av_read_frame(ifmt, p) >= 0) {
        int stream_id = stream_mapping[p->stream_index];
        if (stream_id >= 0 && buf[stream_id] == AV_NOPTS_VALUE) {
            buf[stream_id] = p->pts;
            ++ready_num;
        }
        av_packet_unref(p);
    }
    av_packet_free(&p);
}

static int seek(AVFormatContext* ifmt, int64_t start, int64_t end,
                int* stream_mapping, int valid_stream_num,
                StreamContext* stream_ctxs) {
    int64_t* pts_buf = av_malloc_array(valid_stream_num, sizeof(int64_t));
    int* ready_flags = av_mallocz_array(valid_stream_num, sizeof(int));
    AVPacket* p = NULL;
    int ret = 0;

    ret = avformat_seek_file(ifmt, -1, INT_MIN, end, end, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed seek forwad from end pts %ld", end);
        goto end_seek;
    }
    seek_first_pts(ifmt, stream_mapping, valid_stream_num, pts_buf);
    for (int i = 0; i < valid_stream_num; ++i) {
        stream_ctxs[i].copyEndPts = pts_buf[i];
    }

    ret = avformat_seek_file(ifmt, -1, start, start, INT_MAX, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed seek backward from start pts %ld",
               start);
        goto end_seek;
    }
    seek_first_pts(ifmt, stream_mapping, valid_stream_num, pts_buf);
    for (int i = 0; i < valid_stream_num; ++i) {
        stream_ctxs[i].copyStPts = pts_buf[i];
    }

    for (int i = 0; i < valid_stream_num; ++i) {
        stream_ctxs[i].startPts =
            streamStdScale(&stream_ctxs[i], start, AV_TIME_BASE_Q);
        stream_ctxs[i].endPts =
            streamStdScale(&stream_ctxs[i], end, AV_TIME_BASE_Q);
    }

    ret = avformat_seek_file(ifmt, -1, INT_MIN, start, start, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed seek forward from start pts %ld",
               start);
        goto end_seek;
    }

    p = av_packet_alloc();
    int ready_num = 0;
    while (ready_num < valid_stream_num && av_read_frame(ifmt, p) >= 0) {
        int stream_id = stream_mapping[p->stream_index];
        if (stream_id >= 0) {
            int ready_flag = 0;
            switch (stream_ctxs[stream_id].stream_type) {
            case AVMEDIA_TYPE_VIDEO:
                ready_flag = videoStreamSeekStart(&stream_ctxs[stream_id], p);
                break;
            case AVMEDIA_TYPE_AUDIO:
                ready_flag = audioStreamSeekStart(&stream_ctxs[stream_id], p);
                break;
            default:
                break;
            }
            if (!ready_flags[stream_id] && ready_flag) {
                ready_flags[stream_id] = ready_flag;
                ++ready_num;
            }
        }
        av_packet_unref(p);
    }

    int64_t min_start_pts = start;
    for (int i = 0; i < valid_stream_num; ++i) {
        if (stream_ctxs[i].stream_type == AVMEDIA_TYPE_AUDIO) {
            int64_t s_st_pts = streamStdRescale(
                &stream_ctxs[i], stream_ctxs[i].startPts, AV_TIME_BASE_Q);
            min_start_pts = FFMIN(min_start_pts, s_st_pts);
        }
    }
    for (int i = 0; i < valid_stream_num; ++i) {
        stream_ctxs[i].startPts =
            streamStdScale(&stream_ctxs[i], min_start_pts, AV_TIME_BASE_Q);
        relocatePts(&stream_ctxs[i]);
    }

end_seek:
    av_free(pts_buf);
    av_free(ready_flags);
    av_packet_free(&p);
    return ret;
}

int main(int argc, char* argv[]) {
    int ret;
    int64_t start_pts = 60 * AV_TIME_BASE, end_pts = 90 * AV_TIME_BASE;
    const char* in_filename = "/home/wiennan/桌面/sword_art.mp4";
    const char* out_filename = "/home/wiennan/桌面/vcut_example.mp4";
    AVOutputFormat* ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    StreamContext* stream_ctxs = NULL;
    AVPacket* p_in = av_packet_alloc();
    AVPacket* p_out = av_packet_alloc();
    StreamTranscodeFunc* transFuns = NULL;

    int stream_index = 0;
    int* stream_mapping = NULL;
    int stream_mapping_size = 0;
    int* finished_streams = NULL;

    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not open input file '%s'",
               in_filename);
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Failed to retrieve input stream information");
        goto end;
    }

    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (!ofmt_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    stream_mapping_size = ifmt_ctx->nb_streams;
    stream_mapping =
        av_mallocz_array(stream_mapping_size, sizeof(*stream_mapping));
    stream_ctxs = av_mallocz_array(stream_mapping_size, sizeof(StreamContext));
    if (!stream_mapping || !stream_ctxs) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream* out_stream;
        AVStream* in_stream = ifmt_ctx->streams[i];
        AVCodecParameters* in_codecpar = in_stream->codecpar;

        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            stream_mapping[i] = -1;
            continue;
        }

        stream_mapping[i] = stream_index++;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy codec parameters\n");
            goto end;
        }
        out_stream->codecpar->codec_tag = 0;
    }
    if (stream_index == 0) {
        av_log(NULL, AV_LOG_ERROR, "No valid stream for cutting\n");
        goto end;
    }
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'",
                   out_filename);
            goto end;
        }
    }

    // Write header. Must call before seek
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        goto end;
    }

    // Init cutting context
    for (int i = 0; i < ifmt_ctx->nb_streams; ++i) {
        int stream_id = stream_mapping[i];
        if (stream_id >= 0) {
            // Init Stream Cutting Context
            ret =
                initFromAVStream(&stream_ctxs[stream_id], ifmt_ctx->streams[i],
                                 ofmt_ctx->streams[stream_id]);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR,
                       "Failed init stream cutting context of stream %d: %s\n",
                       i, av_err2str(ret));
                goto end;
            }
        }
    }

    // seek
    ret = seek(ifmt_ctx, start_pts, end_pts, stream_mapping, stream_index,
               stream_ctxs);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when seeking output file\n");
        goto end;
    }

    // cutting
    transFuns = av_malloc_array(stream_index, sizeof(StreamTranscodeFunc));
    for (int i = 0; i < stream_index; ++i) {
        switch (stream_ctxs[i].stream_type) {
        case AVMEDIA_TYPE_AUDIO:
            transFuns[i] = audioTranscode;
            break;
        case AVMEDIA_TYPE_VIDEO:
            transFuns[i] = videoTranscode;
            break;
        default:
            break;
        }
    }
    int finished_num = 0;
    finished_streams = av_mallocz_array(stream_index, sizeof(int));
    while (finished_num < stream_index && av_read_frame(ifmt_ctx, p_in) >= 0) {
        int stream_id = stream_mapping[p_in->stream_index];
        if (stream_id >= 0) {
            StreamContext* s_ctx = &stream_ctxs[stream_id];
            StreamTranscodeFunc func = transFuns[stream_id];
            int t_ret;
            AVPacket* payload = p_in;
            while ((t_ret = func(s_ctx, payload, p_out)) >= 0 ||
                   t_ret == ERR_TRANS_MODE_CHANGE) {
                payload = NULL;
                if (t_ret >= 0) {
                    int w_ret = av_interleaved_write_frame(ofmt_ctx, p_out);
                    if (w_ret < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Error muxing packet: %s\n",
                               av_err2str(w_ret));
                        goto end;
                    }
                }
            }
            if (t_ret == AVERROR_EXIT) {
                goto end;
            }
            if (!finished_streams[stream_id] &&
                s_ctx->error_code == ERR_REACH_ENDPTS) {
                finished_streams[stream_id] = 1;
                ++finished_num;
            }
            av_packet_unref(p_out);
            av_packet_unref(p_in);
        }
    }
    // For the case we're encountering EOF of av_read_frame before all stream
    // reaching its end pts
    if (finished_num < stream_index) {
        for (int i = 0; i < stream_index; ++i) {
            StreamFlushFunc func;
            switch (stream_ctxs[i].stream_type) {
            case AVMEDIA_TYPE_AUDIO:
                func = audioFlush;
                break;
            case AVMEDIA_TYPE_VIDEO:
                func = videoFlush;
                break;
            default:
                continue;
            }
            int f_ret;
            while ((f_ret = func(&stream_ctxs[i], p_out)) >= 0) {
                int w_ret = av_interleaved_write_frame(ofmt_ctx, p_out);
                if (w_ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error muxing packet: %s\n",
                           av_err2str(w_ret));
                    goto end;
                }
            }
            if (f_ret == AVERROR_EXIT) {
                goto end;
            }
        }
    }
    av_write_trailer(ofmt_ctx);

end:
    avformat_close_input(&ifmt_ctx);
    av_packet_free(&p_in);
    av_packet_free(&p_out);
    if (transFuns)
        av_free(transFuns);

    /* close output */
    if (ofmt_ctx) {
        if (!(ofmt->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctx->pb);
        avformat_free_context(ofmt_ctx);
    }

    if (stream_ctxs) {
        /* close cutting context */
        for (int i = 0; i < stream_index; ++i) {
            closeStreamContext(&stream_ctxs[i]);
        }
        av_free(stream_ctxs);
    }

    if (stream_mapping) {
        av_free(stream_mapping);
    }

    if (finished_streams) {
        av_free(finished_streams);
    }

    return 0;
}
