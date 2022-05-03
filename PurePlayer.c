/*
 * A simple video player with stepless (in theory) playing speed change ability.
 * Author: Wang Yingnan
 * Github: Viennan
 * Email: wangynine@163.com
 * Complete at 2022.05.03
 */

#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <getopt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

#define SYSTEM_TIME_BASE 1000000.0
#define REFRESH_RATE 0.01
#define CURSOR_HIDE_DELAY 1000000 // in microsecond

#define MAX_PLAY_SPEED 2
#define MIN_PLAY_SPEED 0.5

#define SDL_AUDIO_BUFFER_SIZE 2048
#define MAX_AUDIO_FRAME_SIZE 192000
#define AUDIO_QUEUE_SIZE 5 * 1024 * 1024
#define VIDEO_QUEUE_SIZE 15 * 1024 * 1024
#define VIDEO_PICTURE_QUEUE_SIZE 50 * 1024 * 1024
#define MAX_ARRAY_QUEUE_SIZE 64 // Must be power of 2
#define MAX_CACHED_PICTURE 3    // Pictures to be shown will not exceeded MAX_CACHED_PICTURE
#define MAX_CACHED_AUDIO_SLICE 5

#define SYNC_THRESHOLD 0.05
#define SYNC_VIDEO_STEP 0.01
#define SYNC_AUDIO_THRESHOLD 0.01
#define SYNC_AUDIO_RESET_THRESHOLD 0.05
#define SYNC_AUDIO_MAX_COMPENSATE 0.1
#define SYNC_AUDIO_DIFF_POOLING_SIZE 10

#define QUEUE_INDEX(x, size) ((x) & ((size)-1))

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)
#define FF_RESET_SPEED_EVENT (SDL_USEREVENT +2)

extern char *optarg;
extern int optind, opterr, optopt;

static int64_t cursor_last_shown_pts;
static int64_t cursor_hidden;

static const struct TextureFormatEntry {
     enum AVPixelFormat format;
     int texture_fmt;
 } sdl_texture_format_map[] = {
     { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
     { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
     { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
     { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
     { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
     { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
     { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
     { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
     { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
     { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
     { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
     { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
     { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
     { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
     { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
     { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
     { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
     { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
     { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
     { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN }, // This entry must be placed at the end
 };

typedef void (*Destructor)(void **);
typedef void (*Filter)(void **);

typedef enum ClockType
{
    AUDIO_CLOCK,
    EXTERNAL_CLOCK,
    AUTO_CLOCK
} ClockType;

typedef struct MediaPacket
{
    AVPacket *p;
    uint seq_id;
} MediaPacket;

// Support YUV420P
typedef struct VideoPicture
{
    AVFrame *pFrame;
    Uint8 *data[8];
    int linesize[8];
    AVRational sar;
    int width;
    int height;
    uint64_t size;
    double sync_pts; // the lastest presentation time in ms
    double sync_duration;
    double pts;
    double duration;
    int seq_id;
    double speed;
} VideoPicture;

typedef struct AudioSlice
{
    Uint16 *data;
    int len;
    int offset;
    int sample_num;
    double sync_pts; // the lastest presentation time in ms
    double pts;
    int seq_id;
    double speed;
} AudioSlice;

typedef struct SyncClock
{
    double pts_diff;
    double pts;
    int paused;
    int seq_id;
} SyncClock;

typedef struct MediaClock
{
    double media_pts;
    double scaled_pts;
    double speed;
} MediaClock;

typedef struct AvgPool
{
    int pool_sz;
    int sz;
    int r_ind;
    int w_ind;
    double data[MAX_ARRAY_QUEUE_SIZE];
    double sum;
    double coeff;
    double coeff_high;
} AvgPool;

typedef struct ListNode
{
    void *dptr;
    uint64_t size;
    struct ListNode *next;
} ListNode;

typedef struct BlockingQueue
{
    ListNode *head;
    ListNode *tail;
    int closed;
    uint64_t size;
    uint64_t max_size;
    SDL_mutex *mutex;
    SDL_cond *get_cond;
    SDL_cond *put_cond;
} BlockingQueue;

typedef struct ArrayBlockingQueue
{
    int r_ind, w_ind;
    void *dptrs[MAX_ARRAY_QUEUE_SIZE];
    int max_size;
    int size;
    int closed;
    SDL_mutex *mutex;
    SDL_cond *get_cond;
    SDL_cond *put_cond;
} ArrayBlockingQueue;

typedef struct MediaDecodeContext
{
    AVStream *pStream;
    AVCodec *pCodec;
    AVCodecContext *pCodecCtx;
    BlockingQueue input_queue;
    MediaPacket *pPacketCache;
    AVFrame *pFrame;
    MediaClock clk;
    int codec_ready;
    int frame_avalible;
    int seq_id;
    int eof;
    double start_pts;
    double tb;
    enum AVMediaType media_type;
} MediaDecodeContext;

typedef enum PlayerStatus
{
    PAUSE = 0,
    Play = 1,
    QUIT = 2
} PlayerStatus;

typedef struct PlayerContext
{
    char filename[512];

    AVFormatContext *pFormatCtx;
    int audio_stream_id, video_stream_id;
    SwrContext *pASwrCtx;
    ArrayBlockingQueue a_play_queue, v_play_queue;
    AudioSlice *pAudioBuf;
    AvgPool audio_diff_pool;

    MediaDecodeContext video_decode_ctx, audio_decode_ctx;
    double speed, min_speed, max_speed;
    int seek_req;
    int64_t seek_pos;
    int64_t seek_flag;

    int screen_width, screen_height;

    SDL_Window *screen;
    int full_screen;
    SDL_Renderer *renderer;
    int texture_fmt;
    SDL_Texture *texture;
    SDL_AudioSpec audio_spec;
    double volume;
    int sdl_mix_volume;
    SDL_Thread *demux_tid, *v_decode_tid, *a_decode_tid;
    SDL_mutex *mutex;

    enum ClockType clock_type;
    SyncClock sync_clock;
    double video_sync_r_speed;
    double audio_lastest_pts, video_lastest_pts, video_latest_sys_pts, video_speed;
    double duration;

    int quit_flag;
    int force_refresh_video;
    int seq_id;
} PlayerContext;

static MediaPacket *create_media_packet(int seq_id)
{
    MediaPacket *mp = av_mallocz(sizeof(MediaPacket));
    mp->p = av_packet_alloc();
    mp->seq_id = seq_id;
    return mp;
}

static MediaPacket *create_flushing_media_packet(int seq_id)
{
    MediaPacket *mp = av_mallocz(sizeof(MediaPacket));
    mp->seq_id = seq_id;
    return mp;
}

static void free_media_packet(MediaPacket **mps)
{
    if (!mps || !(*mps))
        return;
    MediaPacket *mp = *mps;
    av_packet_free(&mp->p);
    *mps = NULL;
}

static void free_video_picture(VideoPicture **ps)
{
    if (!ps || !(*ps))
        return;

    VideoPicture *p = *ps;
    if (p->pFrame) {
        av_frame_free(&p->pFrame);
    } else {
        if (p->data[0])
            av_freep(&p->data[0]);
    }
    av_free(p);
    *ps = NULL;
}

static AudioSlice *create_audio_slice()
{
    return av_mallocz(sizeof(AudioSlice));
}

static void free_audio_slice(AudioSlice **ps)
{
    if (!ps || !(*ps))
        return;

    AudioSlice *p = *ps;
    if (p->data)
        av_free(p->data);
    av_free(p);
    *ps = NULL;
}

static inline void set_sync_clock(SyncClock *clk, double pts)
{
    clk->pts_diff = pts - av_gettime_relative() / SYSTEM_TIME_BASE;
    clk->pts = pts;
}

static void init_sync_clock(SyncClock *clk, double pts, int seq_id)
{
    clk->paused = 0;
    clk->seq_id = seq_id;
    set_sync_clock(clk, pts);
}

static inline double get_sync_clock(SyncClock *clk)
{
    if (clk->paused)
        return clk->pts;
    else
        return clk->pts_diff + av_gettime_relative() / SYSTEM_TIME_BASE;
}

static inline double update_media_clock_pts(MediaClock *clk, double pts, double *speed)
{
    double curr_speed = clk->speed;
    clk->scaled_pts += (pts - clk->media_pts) / curr_speed;
    clk->media_pts = pts;
    *speed = curr_speed;
    return clk->scaled_pts;
}

static inline void init_media_clock(MediaClock *clk, double pts, double speed)
{
    clk->speed = speed;
    clk->scaled_pts = pts;
    clk->media_pts = pts;
}

static inline void set_media_clock_speed(MediaClock *clk, double speed)
{
    clk->speed = speed;
}

static inline void reset_media_clock_pts(MediaClock *clk, double pts)
{
    clk->scaled_pts = pts;
    clk->media_pts = pts;
}

static inline double get_video_pts(PlayerContext *ctx)
{
    double video_pts;
    if (ctx->sync_clock.paused)
        video_pts = ctx->video_lastest_pts;
    else
        video_pts = ctx->video_lastest_pts + (av_gettime_relative() - ctx->video_latest_sys_pts) / SYSTEM_TIME_BASE * ctx->video_speed;
    return FFMIN(video_pts, ctx->duration);
}

static inline void set_video_pts(PlayerContext *ctx, double pts, double speed)
{
    ctx->video_lastest_pts = pts;
    ctx->video_latest_sys_pts = av_gettime_relative();
    ctx->video_speed = speed;
}

static inline double get_media_pts(PlayerContext *ctx)
{
    return FFMIN((FFMAX(get_video_pts(ctx), ctx->audio_lastest_pts)), ctx->duration);
}

static void reset_pool(AvgPool *pool, int pool_sz, double coeff, double coeff_high)
{
    pool->pool_sz = FFMIN(pool_sz, MAX_ARRAY_QUEUE_SIZE - 1);
    pool->r_ind = 0;
    pool->w_ind = pool->pool_sz;
    pool->sum = 0;
    pool->sz = 0;
    pool->coeff = coeff;
    pool->coeff_high = coeff_high;
    memset(&pool->data[0], 0, sizeof(pool->data));
}

static void clear_pool(AvgPool *pool)
{
    pool->sz = 0;
    pool->r_ind = 0;
    pool->w_ind = 0;
    pool->sum = 0;
    memset(&pool->data[0], 0, sizeof(pool->data));
}

static inline double set_pooling_data(AvgPool *pool, double data)
{
    pool->sz = FFMIN(pool->pool_sz, pool->sz + 1);
    pool->sum = data + pool->coeff * pool->sum - pool->coeff_high * pool->data[pool->r_ind];
    pool->data[pool->w_ind] = data;
    pool->r_ind = QUEUE_INDEX(pool->r_ind + 1, MAX_ARRAY_QUEUE_SIZE);
    pool->w_ind = QUEUE_INDEX(pool->w_ind + 1, MAX_ARRAY_QUEUE_SIZE);
    return pool->sum * (1.0 - pool->coeff);
}

static ListNode *create_list_node(void *dptr, uint64_t sz)
{
    ListNode *node = av_mallocz(sizeof(ListNode));
    node->dptr = dptr;
    node->size = sz;
    return node;
}

static void init_blocking_queue(BlockingQueue *q, uint64_t max_size)
{
    q->mutex = SDL_CreateMutex();
    q->put_cond = SDL_CreateCond();
    q->get_cond = SDL_CreateCond();
    q->closed = 0;
    q->head = create_list_node(NULL, 0); // dummy node
    q->tail = q->head;
    q->size = 0;
    q->max_size = max_size;
}

static void _week_up_queue(SDL_mutex *mutex, SDL_cond *put_cond, SDL_cond *get_cond)
{
    SDL_LockMutex(mutex);
    if (put_cond)
    {
        SDL_CondSignal(put_cond);
    }
    if (get_cond)
    {
        SDL_CondSignal(get_cond);
    }
    SDL_UnlockMutex(mutex);
}

static void week_up_blocking_queue(BlockingQueue *q, int put, int get)
{
    _week_up_queue(q->mutex, put ? q->put_cond : NULL, get ? q->get_cond : NULL);
}

static void close_blocking_queue(BlockingQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->closed = 1;
    SDL_CondSignal(q->put_cond);
    SDL_CondSignal(q->get_cond);
    SDL_UnlockMutex(q->mutex);
}

static void clear_data_in_blocking_queue(BlockingQueue *q, Destructor desturctor)
{
    while (q->head != NULL)
    {
        ListNode *node = q->head;
        desturctor(&node->dptr);
        q->head = q->head->next;
        av_free(node);
    }
    q->tail = NULL;
}

static void clear_blocking_queue(BlockingQueue *q, Destructor desturctor)
{
    SDL_LockMutex(q->mutex);
    clear_data_in_blocking_queue(q, desturctor);
    q->head = create_list_node(NULL, 0); // dummy node
    q->tail = q->head;
    q->size = 0;
    SDL_CondSignal(q->put_cond);
    SDL_UnlockMutex(q->mutex);
}

static void free_blocking_queue(BlockingQueue *q, Destructor desturctor)
{
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->put_cond);
    SDL_DestroyCond(q->get_cond);
    clear_data_in_blocking_queue(q, desturctor);
}

static int queue_put_node(BlockingQueue *q, ListNode *node)
{
    int ret = 0;
    SDL_LockMutex(q->mutex);
    if (q->size + node->size > q->max_size && !q->closed)
        SDL_CondWait(q->put_cond, q->mutex);

    if (q->closed)
        ret = AVERROR(EACCES);
    else if (q->size + node->size > q->max_size > q->max_size)
        ret = AVERROR(EAGAIN);
    else
    {
        q->tail->next = node;
        q->tail = node;
        q->size += node->size;
        SDL_CondSignal(q->get_cond);
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int queue_put(BlockingQueue *q, void *dptr, uint64_t sz)
{
    ListNode *node = create_list_node(dptr, sz);
    return queue_put_node(q, node);
}

static int queue_get(BlockingQueue *q, void **dptr)
{
    int ret = 0;
    SDL_LockMutex(q->mutex);
    if (q->head == q->tail && !q->closed)
        SDL_CondWait(q->get_cond, q->mutex);

    if (q->head == q->tail)
    {
        ret = q->closed ? AVERROR(EACCES) : AVERROR(EAGAIN);
    }
    else
    {
        ListNode *node = q->head->next;
        *dptr = node->dptr;
        node->dptr = NULL;
        q->size -= node->size;
        av_free(q->head);
        q->head = node;
        SDL_CondSignal(q->put_cond);
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static inline int queue_size(BlockingQueue *q)
{
    return q->size;
}

static void init_array_blocking_queue(ArrayBlockingQueue *q, int max_size)
{
    SDL_assert(max_size <= MAX_ARRAY_QUEUE_SIZE);
    q->mutex = SDL_CreateMutex();
    q->get_cond = SDL_CreateCond();
    q->put_cond = SDL_CreateCond();
    memset(q->dptrs, 0, sizeof(q->dptrs));
    q->closed = 0;
    q->size = 0;
    q->max_size = max_size;
}

static void close_array_blocking_queue(ArrayBlockingQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->closed = 1;
    SDL_CondSignal(q->get_cond);
    SDL_CondSignal(q->put_cond);
    SDL_UnlockMutex(q->mutex);
}

static void week_up_array_blocking_queue(ArrayBlockingQueue *q, int put, int get)
{
    _week_up_queue(q->mutex, put ? q->put_cond : NULL, get ? q->get_cond : NULL);
}

static void clear_data_in_array_blocking_queue(ArrayBlockingQueue *q, Destructor destructor)
{
    int idx = q->r_ind;
    while (QUEUE_INDEX(idx, MAX_ARRAY_QUEUE_SIZE) != q->w_ind)
    {
        destructor(&q->dptrs[idx]);
        idx = QUEUE_INDEX(idx + 1, MAX_ARRAY_QUEUE_SIZE);
    }
    q->r_ind = 0;
    q->w_ind = 0;
    q->size = 0;
}

static void free_array_blocking_queue(ArrayBlockingQueue *q, Destructor destructor)
{
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->put_cond);
    SDL_DestroyCond(q->get_cond);
    clear_data_in_array_blocking_queue(q, destructor);
}

static int array_queue_put(ArrayBlockingQueue *q, void *dptr)
{
    int ret = 0;
    SDL_LockMutex(q->mutex);
    if (q->size >= q->max_size && !q->closed)
        SDL_CondWait(q->put_cond, q->mutex);

    if (q->closed)
        ret = AVERROR(EACCES);
    else if (q->size >= q->max_size)
        ret = AVERROR(EAGAIN);
    else
    {
        q->dptrs[q->w_ind] = dptr;
        q->w_ind = QUEUE_INDEX(q->w_ind + 1, MAX_ARRAY_QUEUE_SIZE);
        ++q->size;
        SDL_CondSignal(q->get_cond);
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static int array_queue_get(ArrayBlockingQueue *q, void **dptrs, int block)
{
    int ret = 0;
    SDL_LockMutex(q->mutex);
    if (q->r_ind == q->w_ind && !q->closed && block)
        SDL_CondWait(q->get_cond, q->mutex);

    if (q->r_ind != q->w_ind)
    {
        *dptrs = q->dptrs[q->r_ind];
        q->dptrs[q->r_ind] = NULL;
        q->r_ind = QUEUE_INDEX(q->r_ind + 1, MAX_ARRAY_QUEUE_SIZE);
        --q->size;
        SDL_CondSignal(q->put_cond);
    }
    else
    {
        if (q->closed)
            ret = AVERROR(EACCES);
        else
            ret = AVERROR(EAGAIN);
    }
    SDL_UnlockMutex(q->mutex);
    return ret;
}

static inline int array_queue_size(ArrayBlockingQueue *q)
{
    return q->size;
}

// only use it in consumer thread when the queue is single in single out
// do not use in multi out situation
static inline void *array_queue_peek(ArrayBlockingQueue *q)
{
    return q->dptrs[q->r_ind];
}

static inline void *array_queue_next(ArrayBlockingQueue *q)
{
    return q->dptrs[QUEUE_INDEX(q->r_ind + 1, MAX_ARRAY_QUEUE_SIZE)];
}

static void init_media_decode_context(MediaDecodeContext *p, int queue_sz)
{
    if (!p)
        return;

    p->pFrame = av_frame_alloc();
    init_blocking_queue(&p->input_queue, queue_sz);
}

static void close_media_decode_context(MediaDecodeContext *p)
{
    if (!p)
        return;
    free_blocking_queue(&p->input_queue, (Destructor)free_media_packet);
    avcodec_free_context(&p->pCodecCtx);
    av_frame_free(&p->pFrame);
    free_media_packet(&p->pPacketCache);
}

static inline int throw_event(Uint32 event_type, void *user_data)
{
    SDL_Event e;
    e.type = event_type;
    e.user.data1 = user_data;
    return SDL_PushEvent(&e);
}

static int retrive_frame(MediaDecodeContext *ctx, AVFrame **ps)
{
    if (!ctx->frame_avalible)
        return AVERROR(EAGAIN);

    *ps = av_frame_clone(ctx->pFrame);
    ctx->frame_avalible = 0;
    return 0;
}

static int stream_decode(PlayerContext *player_ctx, MediaDecodeContext *ctx)
{
    int ret = 0;
    if (ctx->frame_avalible)
        return AVERROR(EAGAIN);
    else if (ctx->codec_ready)
    {
        ret = avcodec_receive_frame(ctx->pCodecCtx, ctx->pFrame);
        ctx->codec_ready = (ret >= 0);
        if (ret >= 0)
            ctx->frame_avalible = 1;
        ctx->eof = ret == AVERROR_EOF;
    }
    else if (ctx->pPacketCache)
    {
        if (ctx->pPacketCache->seq_id != player_ctx->seq_id)
        {
            free_media_packet(&ctx->pPacketCache);
            return AVERROR(EAGAIN);
        }
        ret = avcodec_send_packet(ctx->pCodecCtx, ctx->pPacketCache->p);
        if (ret >= 0 || ret != AVERROR(EAGAIN))
        {
            free_media_packet(&ctx->pPacketCache);
            if (ret < 0)
                av_log(NULL, AV_LOG_ERROR, "Send Packet Error: %s\n", av_err2str(ret));
        }
        else
        {
            ctx->codec_ready = 1;
        }
    }
    else
    {
        ret = queue_get(&ctx->input_queue, (void **)&ctx->pPacketCache);
        if (ret >= 0 && ctx->seq_id != ctx->pPacketCache->seq_id)
        {
            ctx->seq_id = ctx->pPacketCache->seq_id;
            avcodec_flush_buffers(ctx->pCodecCtx);
            double reset_pts = ctx->start_pts;
            if (reset_pts < 0)
            {
                ctx->tb = av_q2d(ctx->pStream->time_base);
                reset_pts = ctx->pPacketCache->p->pts * ctx->tb;
            }
            reset_media_clock_pts(&ctx->clk, reset_pts);
        }
    }
    return ret;
}

static int _play_queue_put(PlayerContext *ctx, ArrayBlockingQueue *play_queue, void **ps, int seq_id, Destructor destructor)
{
    if (seq_id != ctx->seq_id)
    {
        destructor(ps);
        return AVERROR(EAGAIN);
    }
    int ret = array_queue_put(play_queue, *ps);
    if (ret < 0)
    {
        if (ret != AVERROR(EAGAIN))
        {
            throw_event(SDL_QUIT, ctx);
        }
        return ret;
    }
    *ps = NULL;
    return 0;
}

static int reset_auido_sample_ctx(PlayerContext* ctx, int dst_sample_rate) {
    MediaDecodeContext *pACtx = &ctx->audio_decode_ctx;
    struct SwrContext *sw_ctx = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, dst_sample_rate, 
                                 pACtx->pCodecCtx->channel_layout, pACtx->pCodecCtx->sample_fmt, pACtx->pCodecCtx->sample_rate, 
                                 0, NULL);
    struct SwrContext *tmp_ctx = sw_ctx;
    int ret = swr_init(sw_ctx);
    if (ret < 0)
    {
        av_log(NULL, AV_LOG_WARNING, "Couldn't set audio convert context: %s\n", av_err2str(ret));
        goto clear_swr_ctx;
    }
    tmp_ctx = ctx->pASwrCtx;
    ctx->pASwrCtx = sw_ctx;
clear_swr_ctx:
    if (tmp_ctx) {
        swr_close(tmp_ctx);
        swr_free(&tmp_ctx);
    }
    return ret;
}

static int reset_auido_speed(PlayerContext* ctx, double speed) {
    int new_sample_rate = ctx->audio_decode_ctx.pCodecCtx->sample_rate / speed;
    int ret = reset_auido_sample_ctx(ctx, new_sample_rate);
    if (ret >= 0) {
        set_media_clock_speed(&ctx->audio_decode_ctx.clk, speed);
    }
    return ret;
}

typedef struct AudioDecodeCache {
    Uint8 **data;
    double pts;
    double speed;
    enum AVSampleFormat fmt;
    int channels;
    int nb_samples;
    int threshold;
    int seq_id;
    int max_sample_num;
    int linesize;
    int per_sample_sz;
} AudioDecodeCache;

static inline int audio_cache_get_cached_size(AudioDecodeCache *cache) {
    return cache->channels * cache->nb_samples * cache->per_sample_sz;
}

static inline int audio_cache_get_max_size(AudioDecodeCache *cache) {
    return cache->max_sample_num * cache->per_sample_sz;
}

static void audio_cache_free_buf(AudioDecodeCache* cache) {
    if (cache->data) {
        av_freep(cache->data);
    }
}

static AudioSlice* create_audio_slice_from_cache(PlayerContext *ctx, MediaDecodeContext* pACtx, AudioDecodeCache* cache) {
    if (cache->nb_samples <= 0) {
        return NULL;
    }
    AudioSlice *p = create_audio_slice();
    int buf_size = audio_cache_get_cached_size(cache) / cache->speed;
    int sdl_per_sample_sz = SDL_AUDIO_BITSIZE(AUDIO_S16SYS)>>3;
    int channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);
    int full_sample_sz = channels * sdl_per_sample_sz;
    buf_size = ceil(((double)buf_size) / full_sample_sz) * full_sample_sz;
    int samples_per_channel = buf_size / full_sample_sz;
    p->data = av_malloc(buf_size);
    p->offset = 0;
    int output_samples = swr_convert(
        ctx->pASwrCtx, (uint8_t **)&p->data, samples_per_channel,
        (const uint8_t **)cache->data, cache->nb_samples
    );
    if (output_samples < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to convert audio samples: %s\n", av_err2str(output_samples));
        free_audio_slice(&p);
        throw_event(SDL_QUIT, ctx);
        return NULL;
    }
    p->len = output_samples * full_sample_sz;
    p->sample_num = output_samples;
    p->pts = cache->pts;
    p->sync_pts = update_media_clock_pts(&pACtx->clk, p->pts, &p->speed);
    p->seq_id = cache->seq_id;
    // mark empty
    cache->nb_samples = 0;
    return p;
}

static void audio_cache_realloc_buf(AudioDecodeCache* cache, int linesize) {
    Uint8 **data = cache->data;
    int max_sample_num = linesize / cache->per_sample_sz;
    cache->max_sample_num = max_sample_num;
    cache->linesize = linesize;
    av_samples_alloc_array_and_samples(&cache->data, &linesize, cache->channels, max_sample_num, cache->fmt, 0);
    if (cache->nb_samples > 0) {
        for (int i=0;i<cache->channels;++i) {
            memcpy((void*)cache->data[i], (void*)data[i], cache->nb_samples * cache->per_sample_sz);
        }
    }
    if (data) {
        av_freep(&data[0]);
    }
}

static void audio_cache_append_data(AudioDecodeCache* cache, Uint8 **data, int nb_samples, int linesize) {
    if (nb_samples + cache->nb_samples > cache->max_sample_num) {
        audio_cache_realloc_buf(cache, cache->linesize + linesize * 2);
    }
    int offset = cache->nb_samples * cache->per_sample_sz;
    int sample_sz_per_ch = nb_samples * cache->per_sample_sz;
    for (int i=0;i<cache->channels;++i) {
        memcpy((void*)(cache->data[i]+offset), (void*)data[i], sample_sz_per_ch);
    }
    cache->nb_samples += nb_samples;
}

static int audio_decode_thread(void *arg)
{
    PlayerContext *ctx = (PlayerContext *)arg;
    MediaDecodeContext *pADecodetx = &ctx->audio_decode_ctx;
    AudioSlice *pAudioSlice = NULL;
    init_media_clock(&pADecodetx->clk, 0, ctx->speed);

    int slice_ready = 0;
    AudioDecodeCache cache;
    cache.channels = pADecodetx->pCodecCtx->channels;
    cache.fmt = pADecodetx->pCodecCtx->sample_fmt;
    cache.per_sample_sz = av_get_bytes_per_sample(cache.fmt);
    cache.linesize = 0;
    cache.nb_samples = 0;
    cache.data = NULL;
    cache.seq_id = pADecodetx->seq_id;
    cache.max_sample_num = 0;
    cache.speed = pADecodetx->clk.speed;

    while(!ctx->quit_flag)
    {
        if (ctx->speed != pADecodetx->clk.speed) {
            if (reset_auido_speed(ctx, ctx->speed) < 0) {
                ctx->speed = pADecodetx->clk.speed;
            }
            if (cache.nb_samples == 0) {
                cache.seq_id = pADecodetx->clk.speed;
            }
        }

        int cache_sz = audio_cache_get_cached_size(&cache);
        if (cache_sz > 0) {
            if (cache_sz >= cache.threshold || 
                pADecodetx->seq_id != cache.seq_id || 
                pADecodetx->eof || 
                cache.seq_id != pADecodetx->seq_id) 
            {
                pAudioSlice = create_audio_slice_from_cache(ctx, pADecodetx, &cache);
            }
        }

        if (pAudioSlice) {
            int ret = _play_queue_put(ctx, &ctx->a_play_queue, (void **)&pAudioSlice, pAudioSlice->seq_id, (Destructor)free_audio_slice);
            if (ret >= 0 || !pAudioSlice) {
                slice_ready = 0;
            }
            if (ret < 0 && ret != AVERROR(EAGAIN))
                break;
        }

        AVFrame *pFrame = NULL;
        if (retrive_frame(pADecodetx, &pFrame) >= 0)
        {
            // Check buffer size
            if (cache.nb_samples <= 0) {
                cache.pts = pFrame->best_effort_timestamp * pADecodetx->tb;
                cache.speed = pADecodetx->clk.speed;
                cache.seq_id = pADecodetx->seq_id;
                cache.threshold = ctx->audio_spec.size * pADecodetx->clk.speed;
                cache.nb_samples = 0;
                int curr_max_size = audio_cache_get_cached_size(&cache);
                size_t buf_size = FFMAX(curr_max_size, cache.threshold * 2);
                int sample_sz = av_get_bytes_per_sample(pFrame->format);
                buf_size = FFMAX(buf_size, pFrame->linesize[0] * pFrame->channels * 2);
                buf_size = ceil(((double)buf_size) / (pFrame->channels * sample_sz)) * pFrame->channels * sample_sz;
                if (curr_max_size < buf_size) {
                    audio_cache_realloc_buf(&cache, buf_size / cache.channels);
                }
            }
            audio_cache_append_data(&cache, pFrame->extended_data, pFrame->nb_samples, pFrame->linesize[0]);
            av_frame_free(&pFrame);
        }
        else
        {
            int ret = stream_decode(ctx, pADecodetx);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    continue;
                throw_event(SDL_QUIT, ctx);
                break;
            }
        }
    }
    audio_cache_free_buf(&cache);
    if (pAudioSlice)
        free_audio_slice(&pAudioSlice);
    close_array_blocking_queue(&ctx->a_play_queue);
    return 0;
}

static int decode_video_thread(void *arg)
{
    PlayerContext *ctx = (PlayerContext *)arg;
    MediaDecodeContext *pVDecodetx = &ctx->video_decode_ctx;
    VideoPicture *pVideoPic = NULL;
    AVRational fps = pVDecodetx->pStream->r_frame_rate;
    double r_fps = 1.0 / av_q2d(fps);
    init_media_clock(&pVDecodetx->clk, 0, ctx->speed);
    while (!ctx->quit_flag)
    {
        if (pVideoPic)
        {
            int ret = _play_queue_put(ctx, &ctx->v_play_queue, (void **)&pVideoPic, pVideoPic->seq_id, (Destructor)free_video_picture);
            if (ret < 0 && ret != AVERROR(EAGAIN))
                break;
        }

        AVFrame *pFrame = NULL;
        if (retrive_frame(pVDecodetx, &pFrame) >= 0)
        {
            if (ctx->speed != pVDecodetx->clk.speed) {
                set_media_clock_speed(&pVDecodetx->clk, ctx->speed);
            }
            // load pFrame
            pVideoPic = (VideoPicture *)av_mallocz(sizeof(VideoPicture));
            pVideoPic->width = pFrame->width;
            pVideoPic->height = pFrame->height;
            pVideoPic->sar = pFrame->sample_aspect_ratio;
            pVideoPic->pts = pFrame->best_effort_timestamp * pVDecodetx->tb;
            pVideoPic->duration = r_fps + 0.5 * r_fps * pFrame->repeat_pict;
            pVideoPic->seq_id = pVDecodetx->seq_id;
            pVideoPic->pFrame = pFrame;
            for (int i=0;i<8;++i) {
                pVideoPic->data[i] = pFrame->data[i];
                pVideoPic->linesize[i] = pFrame->linesize[i];
            }

            int frame_size = 0;
            for (int i=0;i<8&&pFrame->buf[i];++i) {
                frame_size += pFrame->buf[i]->size;
            }
            pVideoPic->size = frame_size;
            pFrame = NULL;

            // convert pts and duration to sync time
            pVideoPic->sync_pts = update_media_clock_pts(&pVDecodetx->clk, pVideoPic->pts, &pVideoPic->speed);
            pVideoPic->sync_duration = pVideoPic->duration / pVideoPic->speed;
        }
        else
        {
            int ret = stream_decode(ctx, pVDecodetx);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    continue;
                throw_event(SDL_QUIT, ctx);
                break;
            }
        }
    }
    if (pVideoPic)
        free_video_picture(&pVideoPic);
    // close blocking queue to prevent consumer thread wait forever
    close_array_blocking_queue(&ctx->v_play_queue);
    return 0;
}

static inline double compute_audio_sync_end_pts(AudioSlice *p, double freq)
{
    return p->sync_pts + p->sample_num / freq;
}

static int update_audio_buf(PlayerContext *ctx)
{
    int ret = 0;
    if (!ctx->pAudioBuf || ctx->pAudioBuf->offset >= ctx->pAudioBuf->len)
    {
        if (ctx->pAudioBuf)
            free_audio_slice(&ctx->pAudioBuf);
        ret = array_queue_get(&ctx->a_play_queue, (void **)&ctx->pAudioBuf, 0);
    }
    // Update seq id and sync clock
    if (ret >= 0)
    {
        double end_pts = compute_audio_sync_end_pts(ctx->pAudioBuf, ctx->audio_spec.freq);
        if (ctx->seq_id != ctx->pAudioBuf->seq_id ||
            ctx->seq_id != ctx->sync_clock.seq_id ||
            end_pts < ctx->audio_decode_ctx.start_pts)
        {
            while (ctx->pAudioBuf->seq_id != ctx->seq_id || end_pts < ctx->audio_decode_ctx.start_pts)
            {
                free_audio_slice(&ctx->pAudioBuf);
                ret = array_queue_get(&ctx->a_play_queue, (void **)&ctx->pAudioBuf, 0);
                if (ret < 0)
                    return ret;
                end_pts = compute_audio_sync_end_pts(ctx->pAudioBuf, ctx->audio_spec.freq);
            }
            clear_pool(&ctx->audio_diff_pool);
            SDL_LockMutex(ctx->mutex);
            if (ctx->seq_id != ctx->sync_clock.seq_id)
            {
                set_sync_clock(&ctx->sync_clock, ctx->audio_decode_ctx.start_pts);
                // set_sync_clock_media_pts(&ctx->sync_clock, ctx->audio_decode_ctx.start_pts);
                ctx->audio_lastest_pts = ctx->audio_decode_ctx.start_pts;
                ctx->sync_clock.seq_id = ctx->seq_id;
            }
            SDL_UnlockMutex(ctx->mutex);
        }
    }
    return ret;
}

static int fill_audio_data(PlayerContext *ctx, Uint8 *stream, int len, int start_offset)
{
    int written_len = -start_offset;
    while (update_audio_buf(ctx) >= 0 && written_len < len)
    {
        AudioSlice *p = ctx->pAudioBuf;
        int remain_len = p->len - p->offset;
        int max_written_len = written_len + remain_len;
        if (max_written_len < 0)
        {
            p->offset = p->len;
            written_len = max_written_len;
            continue;
        }
        if (written_len < 0)
            written_len = 0;
        int real_writting_len = FFMIN(remain_len, len - written_len);
        memcpy((void *)(stream + written_len), (Uint8 *)(p->data) + p->offset, real_writting_len);
        p->offset += real_writting_len;
        written_len += real_writting_len;
    }
    return FFMAX(0, written_len);
}

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    PlayerContext *ctx = (PlayerContext *)userdata;
    if (update_audio_buf(ctx) < 0)
    {
        memset((void *)stream, 0, len);
        return;
    }

    if (ctx->sync_clock.paused)
    {
        memset((void *)stream, 0, len);
        return;
    }

    AudioSlice *p = ctx->pAudioBuf;
    SDL_AudioSpec *spec = &ctx->audio_spec;
    int full_sample_size = spec->channels * (SDL_AUDIO_BITSIZE(spec->format) >> 3);
    double bytes_per_second = full_sample_size * spec->freq;
    double audio_sync_pts = p->sync_pts + p->offset / bytes_per_second;
    int offset_compensate = 0;
    int cap = len;
    if (ctx->clock_type != AUDIO_CLOCK)
    {
        double curr_sync_pts = get_sync_clock(&ctx->sync_clock);
        double gap = set_pooling_data(&ctx->audio_diff_pool, audio_sync_pts - curr_sync_pts);
        double abs_gap = fabs(gap);
        double stream_time = len / bytes_per_second;
        if (ctx->audio_diff_pool.sz >= ctx->audio_diff_pool.pool_sz || abs_gap > stream_time)
        {
            if (abs_gap > SYNC_AUDIO_RESET_THRESHOLD || abs_gap > stream_time)
            {
                clear_pool(&ctx->audio_diff_pool);
                if (gap > 0)
                {
                    cap = 0;
                }
                else
                {
                    offset_compensate = len;
                }
            }
            else if (abs_gap > SYNC_AUDIO_THRESHOLD)
            {
                int compensate_len = abs_gap * bytes_per_second;
                if ((double)compensate_len / len > SYNC_AUDIO_MAX_COMPENSATE)
                    compensate_len = len * SYNC_AUDIO_MAX_COMPENSATE;
                compensate_len = (compensate_len / full_sample_size) * full_sample_size;
                if (gap > 0)
                    cap -= compensate_len;
                else
                    offset_compensate = compensate_len;
            }
        }
    }

    int written_len = 0;
    if (ctx->sdl_mix_volume == SDL_MIX_MAXVOLUME)
        written_len = fill_audio_data(ctx, stream, cap, offset_compensate);
    else {
        Uint8 *buf = av_malloc(len);
        written_len = fill_audio_data(ctx, buf, cap, offset_compensate);
        memset(stream, 0, len);
        SDL_MixAudioFormat(stream, buf, ctx->audio_spec.format, written_len, ctx->sdl_mix_volume);
        av_free(buf);
    }
    if (written_len < len)
    {
        if (written_len >= full_sample_size)
        {
            int lastest_sample_offset = written_len - full_sample_size;
            while (written_len < len)
            {
                int cpy_len = FFMIN(full_sample_size, len - written_len);
                memcpy((void *)(stream + written_len), (void *)(stream + lastest_sample_offset), cpy_len);
                written_len += cpy_len;
            }
        }
        if (written_len < len)
            memset((void *)(stream + written_len), 0, len - written_len);
    }
    if (ctx->clock_type == AUDIO_CLOCK)
    {
        set_sync_clock(&ctx->sync_clock, audio_sync_pts);
        if (ctx->pAudioBuf)
        {
            double precise_media_pts = ctx->pAudioBuf->pts + ctx->pAudioBuf->speed * ctx->pAudioBuf->offset / bytes_per_second;
            // set_sync_clock_media_pts(&ctx->sync_clock, precise_media_pts);
            ctx->audio_lastest_pts = precise_media_pts;
        }
    }
}

static VideoPicture *get_refined_peek_picture(ArrayBlockingQueue *q)
{
    if (array_queue_size(q) < 0)
    {
        return NULL;
    }
    VideoPicture *p = array_queue_peek(q);
    if (array_queue_size(q) > 1)
    {
        VideoPicture *next_p = array_queue_next(q);
        if (next_p && next_p->seq_id == p->seq_id)
        {
            p->sync_duration = next_p->sync_pts - p->sync_pts;
            p->duration = next_p->pts - p->pts;
        }
    }
    return p;
}

static double sync_video(VideoPicture **ps, double curr_pts, PlayerContext *ctx, int *show_flag)
{
    VideoPicture *p = get_refined_peek_picture(&ctx->v_play_queue);
    // Check seq id and update sync clock if needed (Mostly for seek operation)
    if (ctx->seq_id != p->seq_id ||
        ctx->seq_id != ctx->sync_clock.seq_id ||
        p->sync_pts + p->sync_duration < ctx->video_decode_ctx.start_pts)
    {
        while (p->seq_id != ctx->seq_id || p->sync_pts + p->sync_duration < ctx->video_decode_ctx.start_pts)
        {
            if (array_queue_get(&ctx->v_play_queue, (void **)ps, 0) < 0)
            {
                *show_flag = 0;
                return REFRESH_RATE;
            }
            free_video_picture(ps);
            p = get_refined_peek_picture(&ctx->v_play_queue);
            if (!p)
            {
                *show_flag = 0;
                return REFRESH_RATE;
            }
        }
        SDL_LockMutex(ctx->mutex);
        if (ctx->clock_type == EXTERNAL_CLOCK && ctx->sync_clock.seq_id != ctx->seq_id)
        {
            set_sync_clock(&ctx->sync_clock, ctx->video_decode_ctx.start_pts);
            set_video_pts(ctx, ctx->video_decode_ctx.start_pts, p->speed);
            ctx->sync_clock.seq_id = ctx->seq_id;
        }
        SDL_UnlockMutex(ctx->mutex);
    }
    if (p->seq_id != ctx->sync_clock.seq_id)
    {
        *show_flag = 0;
        return REFRESH_RATE;
    }
    // Remeber assigning ps!!!
    *ps = p;
    double gap = p->sync_pts - curr_pts;

    double duration = p->sync_duration;
    double video_sync_r_speed = ctx->video_sync_r_speed;
    if (ctx->sync_clock.paused)
    {
        if (p->sync_pts + p->sync_duration < curr_pts)
        {
            *show_flag = 0;
            array_queue_get(&ctx->v_play_queue, (void **)ps, 0);
            duration = 0;
        }
        else
        {
            if (p->sync_pts <= curr_pts)
            {
                *show_flag = 1;
                array_queue_get(&ctx->v_play_queue, (void **)ps, 0);
            }
            else
            {
                *show_flag = 0;
                duration = FFMIN(0.05, gap * 0.5);
            }
        }
    }
    else if (gap > SYNC_THRESHOLD)
    {
        *show_flag = 0;
        duration = FFMIN(0.05, gap * 0.5);
        video_sync_r_speed = FFMAX(1.0, video_sync_r_speed);
    }
    else if (gap < -SYNC_THRESHOLD)
    {
        // skip current frame
        *show_flag = 0;
        duration = 0;
        array_queue_get(&ctx->v_play_queue, (void **)ps, 0);
        video_sync_r_speed = FFMIN(1.0, video_sync_r_speed);
    }
    else
    {
        *show_flag = 1;
        video_sync_r_speed += (gap > 0 ? SYNC_VIDEO_STEP : -SYNC_VIDEO_STEP);
        video_sync_r_speed = FFMAX(0.0, video_sync_r_speed);
        duration *= video_sync_r_speed;
        array_queue_get(&ctx->v_play_queue, (void **)ps, 0);
    }
    ctx->video_sync_r_speed = video_sync_r_speed;
    return duration;
}

static int upload_texture(PlayerContext *ctx, VideoPicture* frame) {
    int access = SDL_TEXTUREACCESS_STREAMING;
    int texture_fmt = ctx->texture_fmt;
    int t_w, t_h;
    int ret = 0;
    if (!ctx->texture || SDL_QueryTexture(ctx->texture, &texture_fmt, &access, &t_w, &t_h) < 0 || 
        t_w != frame->width || t_h != frame->height || texture_fmt != ctx->texture_fmt) 
    {
        if (ctx->texture) {
            SDL_DestroyTexture(ctx->texture);
        }
        ctx->texture = SDL_CreateTexture(ctx->renderer, ctx->texture_fmt, SDL_TEXTUREACCESS_STREAMING,frame->width, frame->height);
        if (!ctx->texture) {
            av_log(NULL, AV_LOG_ERROR, "Failed Create %dx%d texture with %s.\n", frame->width, frame->height, SDL_GetPixelFormatName(ctx->texture_fmt));
            return -1;
        }
        av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", frame->width, frame->height, SDL_GetPixelFormatName(ctx->texture_fmt));
    }

    switch (ctx->texture_fmt)
    {
    case SDL_PIXELFORMAT_IYUV:
        if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
            ret = SDL_UpdateYUVTexture(ctx->texture, NULL, 
                                       frame->data[0], frame->linesize[0], frame->data[1], 
                                       frame->linesize[1], frame->data[2], frame->linesize[2]
                                       );
        } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
            ret = SDL_UpdateYUVTexture(ctx->texture, NULL, 
                                       frame->data[0] + frame->linesize[0] * (frame->height-1), 
                                       -frame->linesize[0],
                                       frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1)-1), 
                                       -frame->linesize[1],
                                       frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1)-1), 
                                       -frame->linesize[2]);
        } else {
            av_log(NULL, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
            return -1;
        }
        break;
    default:
        if (frame->linesize[0] < 0) {
            ret = SDL_UpdateTexture(ctx->texture, NULL, frame->data[0] + frame->linesize[0] * (frame->height - 1), -frame->linesize[0]);
        } else {
            ret = SDL_UpdateTexture(ctx->texture, NULL, frame->data[0], frame->linesize[0]);
        }
        break;
    }
    return ret;
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame) {
#if SDL_VERSION_ATLEAST(2,0,8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 || frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode); /* FIXME: no support for linear transfer */
#endif
 }

 static void calculate_display_rect(SDL_Rect *rect,
                                    int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                    int pic_width, int pic_height, AVRational pic_sar)
 {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0)
        aspect_ratio = av_make_q(1, 1);

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop  + y;
    rect->w = FFMAX((int)width,  1);
    rect->h = FFMAX((int)height, 1);
 }

static void display(PlayerContext *ctx, double *expected_delay)
{
    if (ctx->quit_flag)
        return;

    if (array_queue_size(&ctx->v_play_queue) <= 0) {
        return;
    }

    VideoPicture *p = NULL;
    double curr_pts = get_sync_clock(&ctx->sync_clock);
    double sys_ts = av_gettime_relative();
    int show_flag = 0;
    double delay = sync_video(&p, curr_pts, ctx, &show_flag);
    static int last_show_w = 0, last_show_h = 0;
    static AVRational last_sar;
    if (show_flag) {
        set_sdl_yuv_conversion_mode(p->pFrame);
        upload_texture(ctx, p);
        last_show_w = p->width;
        last_show_h = p->height;
        last_sar = p->sar;
        set_sdl_yuv_conversion_mode(NULL);
        set_video_pts(ctx, p->pts, p->speed);
    }
    if (last_show_w) {
        SDL_RenderClear(ctx->renderer);
        SDL_Rect rect;
        calculate_display_rect(&rect, 0, 0, ctx->screen_width, ctx->screen_height, last_show_w, last_show_h, last_sar);
        SDL_RenderCopyEx(ctx->renderer, ctx->texture, NULL, &rect, 0, NULL, 0);
        SDL_RenderPresent(ctx->renderer);
    }

    if (p && p != array_queue_peek(&ctx->v_play_queue)) {
        free_video_picture(&p);
    }
    delay -= (av_gettime_relative() - sys_ts) / SYSTEM_TIME_BASE;
    *expected_delay = FFMAX(0.0, delay);
    ctx->force_refresh_video = 0;
}

static int init_parser(AVFormatContext *pFormatCtx, int stream_id, AVCodec **psCodec, AVCodecContext **psCodecCtx)
{
    // Find the decoder for the audio stream
    AVCodecParameters *codec_par = pFormatCtx->streams[stream_id]->codecpar;
    AVCodec *pCodec = avcodec_find_decoder(codec_par->codec_id);
    if (pCodec == NULL)
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1; // Codec not found
    }
    // Alloc Codec Context
    AVCodecContext *pCodecCtx = avcodec_alloc_context3(pCodec);
    if (avcodec_parameters_to_context(pCodecCtx, codec_par) < 0)
    {
        fprintf(stderr, "Could not init context of codec %s\n", pCodec->long_name);
        return -1;
    }
    // Open Codec
    int ret = avcodec_open2(pCodecCtx, pCodec, NULL);
    if (ret < 0)
    {
        fprintf(stderr, "Could not open codec %s\n with error: %s\n", pCodec->long_name, av_err2str(ret));
        return -1;
    }

    *psCodec = pCodec;
    *psCodecCtx = pCodecCtx;
    return 0;
}

static int open_audio_stream(PlayerContext *ctx, int stream_id)
{
    // Init audio decoder
    ctx->audio_stream_id = stream_id;
    MediaDecodeContext *pACtx = &ctx->audio_decode_ctx;
    pACtx->pStream = ctx->pFormatCtx->streams[stream_id];
    pACtx->pFrame = av_frame_alloc();
    pACtx->media_type = AVMEDIA_TYPE_AUDIO;
    if (init_parser(ctx->pFormatCtx, ctx->audio_stream_id, &pACtx->pCodec, &pACtx->pCodecCtx) < 0)
    {
        fprintf(stderr, "Failed init audio decoder\n");
        throw_event(SDL_QUIT, ctx);
        return -1;
    }
    pACtx->tb = av_q2d(pACtx->pStream->time_base);

    SDL_AudioSpec wanted_spec;
    wanted_spec.freq = pACtx->pCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 2;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = ctx;
    if (SDL_OpenAudio(&wanted_spec, &ctx->audio_spec) < 0)
    {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    // Init audio resample context
    // Convert any audio format to AUDIO_S16SYS
    int ret = reset_auido_speed(ctx, ctx->speed);
    if (ret < 0) {
        return ret;
    }

    double coeff = exp(log(0.01) / SYNC_AUDIO_DIFF_POOLING_SIZE);
    reset_pool(&ctx->audio_diff_pool, SYNC_AUDIO_DIFF_POOLING_SIZE, coeff, 0.01);
    if (ctx->clock_type == AUTO_CLOCK) {
        ctx->clock_type = AUDIO_CLOCK;
    }
    ctx->a_decode_tid = SDL_CreateThread(audio_decode_thread, "audio_decoder", ctx);
    return 0;
}

static int get_sdl_texture_fmt_by_ff_tag(enum AVPixelFormat pix_fmt) {
    int len = FF_ARRAY_ELEMS(sdl_texture_format_map);
    for (int i=0;i<len-1;++i){
        if (sdl_texture_format_map[i].format == pix_fmt) {
            return sdl_texture_format_map[i].texture_fmt;
        }
    }
    return SDL_PIXELFORMAT_UNKNOWN;
}

static int open_video_stream(PlayerContext *ctx, int stream_id)
{
    // Init video decoder
    ctx->video_stream_id = stream_id;
    MediaDecodeContext *pVCtx = &ctx->video_decode_ctx;
    pVCtx->pStream = ctx->pFormatCtx->streams[stream_id];
    pVCtx->media_type = AVMEDIA_TYPE_VIDEO;
    pVCtx->pFrame = av_frame_alloc();
    if (init_parser(ctx->pFormatCtx, ctx->video_stream_id, &pVCtx->pCodec, &pVCtx->pCodecCtx) < 0)
    {
        fprintf(stderr, "Failed init video decoder\n");
        throw_event(SDL_QUIT, ctx);
        return -1;
    }
    pVCtx->tb = av_q2d(pVCtx->pStream->time_base);

    int texture_fmt = get_sdl_texture_fmt_by_ff_tag(pVCtx->pCodecCtx->pix_fmt);
    if (texture_fmt == SDL_PIXELFORMAT_UNKNOWN) {
        av_log(NULL, AV_LOG_ERROR, "Unspport Video Format: %s\n", av_get_pix_fmt_name(pVCtx->pCodecCtx->pix_fmt));
        throw_event(SDL_QUIT, ctx);
        return -1;
    }
    ctx->texture_fmt = texture_fmt;

    SDL_ShowWindow(ctx->screen);

    if (ctx->clock_type == AUTO_CLOCK) {
        ctx->clock_type = EXTERNAL_CLOCK;
    }
    ctx->v_decode_tid = SDL_CreateThread(decode_video_thread, "video_decoder", ctx);
    return 0;
}

static int open_stream_component(PlayerContext *ctx, enum AVMediaType media_type)
{
    int stream_id = av_find_best_stream(ctx->pFormatCtx, media_type, -1, -1, NULL, 0);
    if (stream_id < 0)
        return AVERROR_STREAM_NOT_FOUND;
    switch (media_type)
    {
    case AVMEDIA_TYPE_AUDIO:
        return open_audio_stream(ctx, stream_id);
    case AVMEDIA_TYPE_VIDEO:
        return open_video_stream(ctx, stream_id);
    }
    return -1;
}

static int demux_thread(void *arg)
{
    PlayerContext *ctx = (PlayerContext *)arg;

    // Open video file
    if (avformat_open_input(&ctx->pFormatCtx, ctx->filename, NULL, NULL) != 0)
    {
        fprintf(stderr, "Could not open %s\n", ctx->filename);
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    // Retrieve stream information
    if (avformat_find_stream_info(ctx->pFormatCtx, NULL) < 0)
    {
        fprintf(stderr, "Could not find stream info in %s\n", ctx->filename);
        throw_event(SDL_QUIT, ctx);
        return -1;
    }
    ctx->duration = (double)ctx->pFormatCtx->duration / AV_TIME_BASE;

    // Dump information about file onto standard error
    av_dump_format(ctx->pFormatCtx, 0, ctx->filename, 0);

    int ret = 0;
    // Open Audio Stream if exist
    // Audio must be open before video for selecting clock type correctly
    ret = open_stream_component(ctx, AVMEDIA_TYPE_AUDIO);
    if (ret < 0)
    {
        if (ret == AVERROR_STREAM_NOT_FOUND)
            fprintf(stdout, "No audio stream in %s\n", ctx->filename);
        else
        {
            fprintf(stderr, "Failed open audio stream");
            throw_event(SDL_QUIT, ctx);
            return -1;
        }
    }

    // Open Video Stream
    ret = open_stream_component(ctx, AVMEDIA_TYPE_VIDEO);
    if (ret < 0)
    {
        fprintf(stderr, "Could not find video stream in %s\n", ctx->filename);
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    MediaPacket *mp = NULL;
    ret = 0;
    int eof_flag = 0;
    MediaDecodeContext *pACtx = &ctx->audio_decode_ctx;
    MediaDecodeContext *pVCtx = &ctx->video_decode_ctx;
    pACtx->seq_id = ctx->seq_id;
    pVCtx->seq_id = ctx->seq_id;
    while(!ctx->quit_flag)
    {
        // Processing seek requirement
        if (ctx->seek_req)
        {
            ctx->seek_req = 0;
            int seek_ret = avformat_seek_file(ctx->pFormatCtx, -1, INT64_MIN, ctx->seek_pos, INT64_MAX, ctx->seek_flag);
            if (seek_ret < 0)
            {
                av_log(NULL, AV_LOG_WARNING, "Failed To Seek File: %s", av_err2str(seek_ret));
                continue;
            }
            if (mp)
            {
                free_media_packet(&mp);
            }
            // Update sequence id
            ctx->seq_id++;

            // Set New Seq Start Pts
            double start_pts = (ctx->seek_flag & AVSEEK_FLAG_BYTE) ? -1.0 : ctx->seek_pos / AV_TIME_BASE;
            pACtx->start_pts = start_pts;
            pVCtx->start_pts = start_pts;

            // Flush blocking queue
            clear_blocking_queue(&pACtx->input_queue, (Destructor)free_media_packet);
            clear_blocking_queue(&pVCtx->input_queue, (Destructor)free_media_packet);
            // Week up decode thread
            week_up_array_blocking_queue(&ctx->a_play_queue, 1, 0);
            week_up_array_blocking_queue(&ctx->v_play_queue, 1, 0);
        }

        if (mp && mp->seq_id != ctx->seq_id)
        {
            free_media_packet(&mp);
        }
        if (!mp)
        {
            mp = create_media_packet(ctx->seq_id);
        }
        ret = av_read_frame(ctx->pFormatCtx, mp->p);
        if (ret < 0)
        {
            if (ret == AVERROR_EOF)
            {
                if (!eof_flag)
                {
                    // Flush Decoder
                    queue_put(&pACtx->input_queue, create_flushing_media_packet(ctx->seq_id), 0);
                    queue_put(&pVCtx->input_queue, create_flushing_media_packet(ctx->seq_id), 0);
                    eof_flag = 1;
                }
                av_usleep(REFRESH_RATE * 1000000.0);
                continue;
            }
            else
            {
                fprintf(stderr, "Demux Error: %s\n", av_err2str(ret));
                throw_event(SDL_QUIT, ctx);
                break;
            }
        }
        eof_flag = 0;

        if (mp && mp->p->stream_index == ctx->audio_stream_id)
        {
            ret = queue_put(&pACtx->input_queue, mp, mp->p->size);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN))
                    continue;
                break;
            }
            mp = NULL;
        }

        if (mp && mp->p->stream_index == ctx->video_stream_id)
        {
            ret = queue_put(&pVCtx->input_queue, mp, mp->p->size);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN))
                    continue;
                break;
            }
            mp = NULL;
        }

        if (mp)
            free_media_packet(&mp);
    }
    free_media_packet(&mp);

    // close blocking queue to prevent consumer thread wait forever
    close_blocking_queue(&pACtx->input_queue);
    close_blocking_queue(&pVCtx->input_queue);

    // close Format Context
    avformat_close_input(&ctx->pFormatCtx);
    return 0;
}

static void free_player_context(PlayerContext **ctx_s)
{
    if (ctx_s == NULL || *ctx_s == NULL)
        return;

    PlayerContext *ctx = *ctx_s;
    ctx->quit_flag = 1;

    SDL_PauseAudio(1);
    close_blocking_queue(&ctx->audio_decode_ctx.input_queue);
    close_blocking_queue(&ctx->video_decode_ctx.input_queue);
    close_array_blocking_queue(&ctx->a_play_queue);
    close_array_blocking_queue(&ctx->v_play_queue);
    int status;
    SDL_WaitThread(ctx->demux_tid, &status);
    SDL_WaitThread(ctx->v_decode_tid, &status);
    SDL_WaitThread(ctx->a_decode_tid, &status);
    SDL_DestroyMutex(ctx->mutex);

    SDL_DestroyTexture(ctx->texture);
    SDL_DestroyRenderer(ctx->renderer);
    SDL_DestroyWindow(ctx->screen);
    SDL_Quit();

    free_array_blocking_queue(&ctx->a_play_queue, (Destructor)free_audio_slice);
    free_array_blocking_queue(&ctx->v_play_queue, (Destructor)free_video_picture);

    if (ctx->pASwrCtx)
    {
        swr_close(ctx->pASwrCtx);
        swr_free(&ctx->pASwrCtx);
    }

    free_audio_slice(&ctx->pAudioBuf);
    close_media_decode_context(&ctx->audio_decode_ctx);
    close_media_decode_context(&ctx->video_decode_ctx);
    avformat_close_input(&ctx->pFormatCtx);

    *ctx_s = NULL;
}

static void play(PlayerContext *ctx)
{
    if (!ctx)
        return;

    ctx->demux_tid = SDL_CreateThread(demux_thread, "demuxer", ctx);
    init_sync_clock(&ctx->sync_clock, 0, ctx->seq_id);
    if (ctx->audio_stream_id >= 0)
    {
        while (array_queue_size(&ctx->a_play_queue) == 0)
            SDL_Delay(1);
        SDL_PauseAudio(0);
    }
    while (array_queue_size(&ctx->v_play_queue) == 0)
        SDL_Delay(1);
}

static int init_sdl(PlayerContext *ctx)
{
    // Init SDL2
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);

    // Set SDL2 Parameters to show video frame
    char window_name[1024];
    sprintf(window_name, "PurePlayer - %s", ctx->filename);
    Uint32 flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
    ctx->screen = SDL_CreateWindow(
        window_name,
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        ctx->screen_width,
        ctx->screen_height,
        flags);
    if (!ctx->screen)
    {
        fprintf(stderr, "Failed to create SDL2 Window\n");
        return -1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

    ctx->renderer = SDL_CreateRenderer(ctx->screen, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ctx->renderer)
    {
        fprintf(stderr, "SDL: could not create renderer: %s\n", SDL_GetError());
        return -1;
    }
    ctx->texture = NULL;
    return 0;
}

static int init_player_context(PlayerContext *ctx)
{
    if (!ctx)
        return -1;

    int ret = init_sdl(ctx);
    if (ret < 0)
    {
        fprintf(stderr, "Failed Init SDL2 part of player context\n");
        free_player_context(&ctx);
        return ret;
    }

    init_media_decode_context(&ctx->audio_decode_ctx, AUDIO_QUEUE_SIZE);
    init_media_decode_context(&ctx->video_decode_ctx, VIDEO_QUEUE_SIZE);
    init_array_blocking_queue(&ctx->a_play_queue, MAX_CACHED_AUDIO_SLICE);
    init_array_blocking_queue(&ctx->v_play_queue, MAX_CACHED_PICTURE);

    ctx->mutex = SDL_CreateMutex();
    ctx->video_sync_r_speed = 1.0;

    return 0;
}

static void VideoRefreshLoop(PlayerContext *ctx, SDL_Event *event)
{
    double expected_delay = 0.0;
    double remaining_time = 0.0;
    int64_t ts = av_gettime_relative();
    SDL_PumpEvents();
    while (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT))
    {
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown_pts > CURSOR_HIDE_DELAY) {
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        if (remaining_time > 0.0)
        {
            av_usleep((int64_t)(FFMIN(remaining_time, REFRESH_RATE) * 1000000.0));
            remaining_time = expected_delay - (av_gettime_relative() - ts) / SYSTEM_TIME_BASE;
        }
        if (remaining_time <= 0.0 || ctx->force_refresh_video)
        {
            expected_delay = REFRESH_RATE;
            display(ctx, &expected_delay);
            remaining_time = expected_delay;
            ts = av_gettime_relative();
        }
        SDL_PumpEvents();
    }
}

static void toggle_pause(PlayerContext *ctx)
{
    set_sync_clock(&ctx->sync_clock, get_sync_clock(&ctx->sync_clock));
    set_video_pts(ctx, get_video_pts(ctx), ctx->video_speed);
    ctx->sync_clock.paused = !ctx->sync_clock.paused;
}

static void log_seek_time(double t)
{
    int64_t t_ms = t * 1000;
    int hh = t_ms / 3600000;
    int remainder = t_ms - hh * 3600000;
    int mm = remainder / 60000;
    remainder -= mm * 60000;
    int ss = remainder / 1000;
    int ms = remainder - ss * 1000;
    av_log(NULL, AV_LOG_INFO, "Seek to %dh:%02dm:%02ds.%03dms \n", hh, mm, ss, ms);
}

static void toggle_seek(PlayerContext *ctx, int64_t pos, int flag)
{
    if (ctx->seek_req || ctx->seq_id != ctx->sync_clock.seq_id)
        return;

    ctx->seek_pos = pos;
    ctx->seek_flag = flag;
    ctx->seek_req = 1;
    week_up_blocking_queue(&ctx->audio_decode_ctx.input_queue, 1, 0);
    week_up_blocking_queue(&ctx->video_decode_ctx.input_queue, 1, 0);
    return;
}

static inline void toggle_seek_by_time(PlayerContext *ctx, double base, int incr)
{
    double target_time = FFMAX(base + incr, 0.0);
    target_time = FFMAX(FFMIN(target_time, ctx->duration), 0.0);
    log_seek_time(target_time);
    toggle_seek(ctx, (int64_t)target_time * AV_TIME_BASE, 0);
    return;
}

static inline void toggle_seek_by_time_absolutely(PlayerContext *ctx, double target_time)
{
    toggle_seek_by_time(ctx, 0.0, target_time);
}

static inline void toggle_seek_by_time_relatively(PlayerContext *ctx, double incr)
{
    toggle_seek_by_time(ctx, get_media_pts(ctx), incr);
}

static inline void toggle_speed_change(PlayerContext *ctx, double base, double incr) {
    ctx->speed = FFMAX(FFMIN(base + incr, ctx->max_speed), ctx->min_speed);
    av_log(NULL, AV_LOG_INFO, "Set player speed to x%.2f\n", ctx->speed);
}

static inline void toggle_full_screen(PlayerContext *ctx, int flag) {
    ctx->full_screen = flag;
    SDL_SetWindowFullscreen(ctx->screen, flag ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    ctx->force_refresh_video = 1;
}

static inline void toggle_volume_change(PlayerContext* ctx, double base, double incr) {
    ctx->volume = av_clipd(base + incr, 0.0, 1.0);
    ctx->sdl_mix_volume = ctx->volume * SDL_MIX_MAXVOLUME;
    av_log(NULL, AV_LOG_INFO, "Set volume to %2.0f%%\n", ctx->volume * 100);
}

static void process_key_down(PlayerContext *ctx, SDL_Event *event)
{
    switch (event->key.keysym.sym)
    {
    case SDLK_p:
    case SDLK_SPACE:
        toggle_pause(ctx);
        break;
    case SDLK_LEFT:
        toggle_seek_by_time_relatively(ctx, -10.0);
        break;
    case SDLK_RIGHT:
        toggle_seek_by_time_relatively(ctx, 10.0);
        break;
    case SDLK_UP:
        toggle_seek_by_time_relatively(ctx, 60.0);
        break;
    case SDLK_DOWN:
        toggle_seek_by_time_relatively(ctx, -60.0);
        break;
    case SDLK_a:
        toggle_speed_change(ctx, ctx->speed, 0.05);
        break;
    case SDLK_s:
        toggle_speed_change(ctx, ctx->speed, -0.05);
        break;
    case SDLK_1:
        toggle_speed_change(ctx, 1.0, 0.0);
        break;
    case SDLK_f:
        toggle_full_screen(ctx, !ctx->full_screen);
        break;
    case SDLK_ESCAPE:
        toggle_full_screen(ctx, 0);
        break;
    case SDLK_9:
        toggle_volume_change(ctx, ctx->volume, 0.01);
        break;
    case SDLK_0:
        toggle_volume_change(ctx, ctx->volume, -0.01);
        break;
    case SDLK_8:
        toggle_volume_change(ctx, 1.0, 0.0);
        break;
    default:
        break;
    }
}

static void process_mouse_button_down(PlayerContext *ctx, SDL_Event *event) {
    if (event->button.button == SDL_BUTTON_LEFT)
    {
        static int64_t last_left_button_down_pts = 0;
        int64_t curr_pts = av_gettime_relative();
        if (curr_pts - last_left_button_down_pts < 500000) {
            toggle_full_screen(ctx, !ctx->full_screen);
        }
        last_left_button_down_pts = curr_pts;
    }
    else if (event->button.button == SDL_BUTTON_RIGHT)
    {
        double percent = (double)event->button.x / ctx->screen_width;
        av_log(NULL, AV_LOG_INFO, "Absolutely seek to %2.0f%% of total duration\n", percent * 100);
        double target_time = percent * ctx->duration;
        toggle_seek_by_time_absolutely(ctx, target_time);
    }
}

static void process_mouse_montion(PlayerContext *ctx, SDL_Event *event) {
    if (cursor_hidden) {
        SDL_ShowCursor(1);
        cursor_hidden = 0;
    }
    cursor_last_shown_pts = av_gettime_relative();
}

static struct option Opts[] = {
    {"clock", required_argument, NULL, 'c'},
    {"scale", required_argument, NULL, 's'},
    {0, 0, 0, 0}};

static void set_clock_type_by_option(PlayerContext *ctx, const char *opt)
{
    if (strcmp("audio", opt) == 0) {
        ctx->clock_type = AUDIO_CLOCK;
    }
    else if (strcmp("external", opt) == 0) {
        ctx->clock_type = EXTERNAL_CLOCK;
    }
    else {
        ctx->clock_type = AUTO_CLOCK;
    }
}

static void set_window_scale_by_option(PlayerContext *ctx, const char *opt)
{
    int width, height;
    int ret = sscanf(opt, "%d:%d", &width, &height);
    if (ret == 2)
    {
        ctx->screen_width = width;
        ctx->screen_height = height;
    }
}

static void parse_options(int argc, char *argv[], PlayerContext *ctx)
{
    int c;
    int digit_optind = 0;
    for (;;)
    {
        int this_option_optind = optind ? optind : 1;
        int option_index = 0;
        c = getopt_long(argc - 1, argv, "c:s:", Opts, &option_index);
        if (c == -1)
            break;
        switch (c)
        {
        case 'c':
            set_clock_type_by_option(ctx, optarg);
            break;
        case 's':
            set_window_scale_by_option(ctx, optarg);
            break;
        default:
            break;
        }
    }
    // Input file is always the last one
    strcpy(ctx->filename, argv[argc - 1]);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "You should input a video file path\n");
        return -1;
    }

    PlayerContext *ctx = av_mallocz(sizeof(PlayerContext));
    ctx->screen_width = 1280;
    ctx->screen_height = 720;
    ctx->clock_type = AUTO_CLOCK;
    ctx->speed = 1.0;
    ctx->min_speed = MIN_PLAY_SPEED;
    ctx->max_speed = MAX_PLAY_SPEED;
    ctx->volume = 1.0;
    ctx->sdl_mix_volume = SDL_MIX_MAXVOLUME;
    // parse command line and set some options in player context
    parse_options(argc, argv, ctx);

    if (init_player_context(ctx) < 0)
        return -1;

    play(ctx);

    SDL_Event event;
    while (ctx && !ctx->quit_flag)
    {
        VideoRefreshLoop(ctx, &event);
        switch (event.type)
        {
        case SDL_KEYDOWN:
            process_key_down(ctx, &event);
            break;
        case SDL_MOUSEBUTTONDOWN:
            process_mouse_button_down(ctx, &event);
            break;
        case SDL_MOUSEMOTION:
            process_mouse_montion(ctx, &event);
            break;
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            free_player_context(&ctx);
            break;
        case FF_REFRESH_EVENT:
            ctx->force_refresh_video = 1;
            break;
        case SDL_WINDOWEVENT:
             switch (event.window.event) {
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                ctx->screen_width  = event.window.data1;
                ctx->screen_height = event.window.data2;
                ctx->force_refresh_video = 1;
                break;
             }
             break;
        default:
            break;
        }
    }
    free_player_context(&ctx);
    return 0;
}
