#include <stdio.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>

#define SYSTEM_TIME_BASE 1000000.0

#define SDL_AUDIO_BUFFER_SIZE 2048
#define MAX_AUDIO_FRAME_SIZE 192000
#define AUDIO_QUEUE_SIZE 5 * 1024 * 1024
#define VIDEO_QUEUE_SIZE 50 * 1024 * 1024
#define VIDEO_PICTURE_QUEUE_SIZE 50 * 1024 * 1024
#define MAX_ARRAY_QUEUE_SIZE 64 // Must be power of 2
#define MAX_CACHED_PICTURE 3 // Pictures to be shown will not exceeded MAX_CACHED_PICTURE
#define MAX_CACHED_AUDIO_SLICE 5

#define SYNC_THRESHOLD 0.05
#define SYNC_VIDEO_STEP 0.01
// #define SYNC_VIDEO_DIFF_POOLING_SIZE 5
#define SYNC_AUDIO_THRESHOLD 0.01
#define SYNC_AUDIO_RESET_THRESHOLD 0.5
#define SYNC_AUDIO_MAX_COMPENSATE 0.1
#define SYNC_AUDIO_DIFF_POOLING_SIZE 10

// #define QUEUE_INDEX(x) ((x) & (MAX_ARRAY_QUEUE_SIZE-1))
#define QUEUE_INDEX(x, size) ((x) & ((size)-1))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)


static int sync_records[7] = {0};

static double total_gap = 0.0;
static double total_video_speed = 0.0;
static int frame_num = 0;

typedef void (*Destructor) (void**);

typedef enum ClockType {
    AUDIO_CLOCK,
    EXTERNAL_CLOCK
} ClockType;

// Support YUV420P
typedef struct VideoPicture {
    Uint8 *data[4];
    int linesize[4];
    int width;
    int height;
    uint64_t size;
    double sync_pts; // the lastest presentation time in ms
    double sync_duration;
    double pts;
    double duration;
} VideoPicture;

typedef struct AudioSlice {
    Uint16 *data;
    int len;
    int offset;
    int sample_rate;
    int channel_num;
    double sync_pts; // the lastest presentation time in ms
    double pts;
} AudioSlice;

typedef struct Clock {
    double ts;  // start time point
    double sys_ts;  // system time stamp
    double speed;  // time spending speed in player view
} Clock;

typedef struct SyncClock {
    double pts_diff;
    double pts;
    int paused;
} SyncClock;

typedef struct MediaClock {
    double media_pts;
    double scaled_pts;
    double speed;
    double r_speed;
} MediaClock;

typedef struct AvgPool {
    int pool_sz;
    int sz;
    int r_ind;
    int w_ind;
    double data[MAX_ARRAY_QUEUE_SIZE];
    double sum;
    double coeff;
    double coeff_high;
} AvgPool;

typedef struct ListNode {
    void *dptr;
    uint64_t size;
    struct ListNode* next;
} ListNode;

typedef struct BlockingQueue {
    ListNode* head;
    ListNode* tail;
    int closed;
    uint64_t size;
    uint64_t max_size;
    SDL_mutex *mutex;
    SDL_cond  *get_cond;
    SDL_cond  *put_cond;
} BlockingQueue;

typedef struct ArrayBlockingQueue {
    int r_ind, w_ind;
    void *dptrs[MAX_ARRAY_QUEUE_SIZE];
    int max_size;
    int size;
    int closed;
    SDL_mutex *mutex;
    SDL_cond  *get_cond;
    SDL_cond  *put_cond;
} ArrayBlockingQueue;

typedef struct MediaDecodeContext {
    enum AVMediaType media_type;
    AVStream* pStream; 
    AVCodec* pCodec;
    AVCodecContext* pCodecCtx;
    BlockingQueue input_queue;
    AVPacket* pPacketCache;
    AVFrame* pFrame;
    MediaClock clk;
    int codec_ready;
    int frame_avalible;
} MediaDecodeContext;

typedef enum PlayerStatus {
    PAUSE = 0,
    Play = 1,
    QUIT = 2
} PlayerStatus;

typedef struct PlayerContext {
    char                    filename[512];

    AVFormatContext         *pFormatCtx;
    int                     audio_stream_id, video_stream_id;
    SwrContext              *pASwrCtx;
    ArrayBlockingQueue      a_play_queue, v_play_queue;
    int                     frame_interval;
    double                  pre_audio_sync_pts;
    AudioSlice              *pAudioBuf;
    AvgPool                 audio_diff_pool;

    MediaDecodeContext      video_decode_ctx, audio_decode_ctx;

    int                     screen_width, screen_height;
    int                     picture_width, picture_height;
    enum AVPixelFormat      display_pix_fmt;
    struct SwsContext       *sws_ctx;

    SDL_Window              *screen;
    SDL_Renderer            *renderer;
    SDL_Texture             *texture;
    SDL_AudioSpec           audio_spec;
    SDL_Thread              *demux_tid, *v_decode_tid, *a_decode_tid;
    SDL_mutex               *mutex;

    enum ClockType          clock_type;
    SyncClock               sync_clock;
    double                  video_sync_r_speed;

    PlayerStatus            status;
} PlayerContext;

static void free_video_picture(VideoPicture **ps)
{
    if (!ps || !(*ps))
        return;

    VideoPicture *p = *ps;
    if (p->data[0])
        av_freep(&p->data[0]);
    av_free(p);
    *ps = NULL;
}

static AudioSlice* create_audio_slice()
{
    return av_mallocz(sizeof(AudioSlice));
}

static void free_auido_slice(AudioSlice **ps)
{
    if (!ps || !(*ps))
        return;

    AudioSlice *p = *ps;
    if (p->data)
        av_free(p->data);
    av_free(p);
    *ps = NULL;
}

static void reset_clock(Clock* c)
{
    c->ts = 0;
    c->speed = 1;
    c->sys_ts = av_gettime_relative();
}

static inline double get_current_time(const Clock *c)
{
    return c->ts + (av_gettime_relative()-c->sys_ts) / SYSTEM_TIME_BASE * c->speed;
}

static inline double get_current_ts(const Clock* c)
{
    return c->ts;
}

static inline void set_clock(Clock* c, uint64_t pts, double speed)
{
    double sys_ts = av_gettime_relative();
    c->ts += (sys_ts - c->sys_ts) * c->speed;
    c->speed = speed;
    c->sys_ts = sys_ts;
}

static inline void set_sync_clock(SyncClock* clk, double pts)
{
    if (!clk->paused)
    {
        clk->pts_diff = pts - av_gettime_relative() / SYSTEM_TIME_BASE;
        clk->pts = pts;
    }
}

static void init_sync_clock(SyncClock* clk, double pts)
{
    clk->paused = 0;
    set_sync_clock(clk, pts);
}

static inline double get_sync_clock(SyncClock* clk)
{
    if (clk->paused)
        return clk->pts;
    else
        return clk->pts_diff + av_gettime_relative() / SYSTEM_TIME_BASE;
}

static inline double update_media_clock_pts(MediaClock* clk, double pts)
{
    clk->scaled_pts += (pts - clk->media_pts) * clk->r_speed;
    clk->media_pts = pts;
    return clk->scaled_pts;
}

static void init_media_clock(MediaClock* clk, double pts)
{
    clk->speed = 1.0;
    clk->r_speed = 1.0;
    clk->scaled_pts = pts;
    clk->media_pts = pts;
}

static void reset_pool(AvgPool* pool, int pool_sz, double coeff, double coeff_high)
{
    pool->pool_sz = MIN(pool_sz, MAX_ARRAY_QUEUE_SIZE - 1);
    pool->r_ind = 0;
    pool->w_ind = pool->pool_sz;
    pool->sum = 0;
    pool->sz = 0;
    pool->coeff = coeff;
    pool->coeff_high = coeff_high;
    memset(&pool->data[0], 0, sizeof(pool->data));
}

static void clear_pool(AvgPool* pool)
{
    pool->sz = 0;
    pool->r_ind = 0;
    pool->w_ind = 0;
    pool->sum = 0;
    memset(&pool->data[0], 0, sizeof(pool->data));
}

static inline double set_pooling_data(AvgPool* pool, double data)
{
    pool->sz = MIN(pool->pool_sz, pool->sz+1);
    pool->sum = data + pool->coeff * pool->sum - pool->coeff_high * pool->data[pool->r_ind];
    pool->data[pool->w_ind] = data;
    pool->r_ind = QUEUE_INDEX(pool->r_ind+1, MAX_ARRAY_QUEUE_SIZE);
    pool->w_ind = QUEUE_INDEX(pool->w_ind+1, MAX_ARRAY_QUEUE_SIZE);
    return pool->sum * (1.0 - pool->coeff);
}

static ListNode* create_list_node(void* dptr, uint64_t sz)
{
    ListNode* node = av_mallocz(sizeof(ListNode));
    node->dptr = dptr;
    node->size = sz;
    return node;
}

static void init_blocking_queue(BlockingQueue* q, uint64_t max_size)
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

static void close_blocking_queue(BlockingQueue* q)
{
    SDL_LockMutex(q->mutex);
    q->closed = 1;
    SDL_CondSignal(q->put_cond);
    SDL_CondSignal(q->get_cond);
    SDL_UnlockMutex(q->mutex);
}

static void clear_data_in_blocking_queue(BlockingQueue* q, Destructor desturctor)
{
    while(q->head != NULL)
    {
        ListNode* node = q->head;
        desturctor(&node->dptr);
        q->head = q->head->next;
        av_free(node);
    }
    q->tail = NULL;
}

static void clear_blocking_queue(BlockingQueue* q, Destructor desturctor)
{
    SDL_LockMutex(q->mutex);
    clear_data_in_blocking_queue(q, desturctor);
    q->head = create_list_node(NULL, 0); // dummy node
    q->tail = q->head;
    q->size = 0;
    SDL_CondSignal(q->put_cond);
    SDL_UnlockMutex(q->mutex);
}

static void free_blocking_queue(BlockingQueue* q, Destructor desturctor)
{
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->put_cond);
    SDL_DestroyCond(q->get_cond);
    clear_data_in_blocking_queue(q, desturctor);
}

static int queue_put_node(BlockingQueue* q, ListNode* node)
{
    int ret = 0;
    SDL_LockMutex(q->mutex);
    while(q->size + node->size > q->max_size && !q->closed)
        SDL_CondWait(q->put_cond, q->mutex);

    if (q->closed)
        ret = AVERROR(EACCES);
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

static int queue_put(BlockingQueue* q, void* dptr, uint64_t sz)
{
    ListNode* node = create_list_node(dptr, sz);
    return queue_put_node(q, node);
}

static int queue_get(BlockingQueue* q, void **dptr)
{
    int ret = 0;
    SDL_LockMutex(q->mutex);
    while(q->head == q->tail && !q->closed)
        SDL_CondWait(q->get_cond, q->mutex);

    if (q->closed && q->head == q->tail)
        ret = AVERROR(EACCES);

    if (q->head != q->tail)
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

static inline int queue_size(BlockingQueue* q)
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

static void clear_data_in_array_blocking_queue(ArrayBlockingQueue *q, Destructor destructor)
{
    int idx = q->r_ind;
    while(QUEUE_INDEX(idx, MAX_ARRAY_QUEUE_SIZE) != q->w_ind)
    {
        destructor(&q->dptrs[idx]);
        idx = QUEUE_INDEX(idx+1, MAX_ARRAY_QUEUE_SIZE);
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
    while(q->size >= q->max_size && !q->closed)
        SDL_CondWait(q->put_cond, q->mutex);
    
    if (q->closed)
        ret = AVERROR(EACCES);
    else
    {
        q->dptrs[q->w_ind] = dptr;
        q->w_ind = QUEUE_INDEX(q->w_ind+1, MAX_ARRAY_QUEUE_SIZE);
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
    while(q->r_ind == q->w_ind && !q->closed && block)
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
        if (!block && !q->closed)
            ret = AVERROR(EAGAIN);
        else
            ret = AVERROR(EACCES);
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
static inline void* array_queue_peek(ArrayBlockingQueue *q)
{
    return q->dptrs[q->r_ind];
}

static inline void* array_queue_next(ArrayBlockingQueue *q)
{
    return q->dptrs[QUEUE_INDEX(q->r_ind+1, MAX_ARRAY_QUEUE_SIZE)];
}

static void init_media_decode_context(MediaDecodeContext *p, int queue_sz)
{
    if (!p)
        return;

    p->pFrame = av_frame_alloc();
    init_blocking_queue(&p->input_queue, queue_sz);
}

static void close_media_decode_context(MediaDecodeContext* p)
{
    if (!p)
        return;
    free_blocking_queue(&p->input_queue, (Destructor)av_packet_free);
    avcodec_free_context(&p->pCodecCtx);
    av_frame_free(&p->pFrame);
    av_packet_free(&p->pPacketCache);
}

static int read_player_status(PlayerContext* ctx)
{
    SDL_LockMutex(ctx->mutex);
    PlayerStatus status = ctx->status;
    SDL_UnlockMutex(ctx->mutex);
    return status;
}

static inline int throw_event(Uint32 event_type, void* user_data)
{
    SDL_Event e;
    e.type = event_type;
    e.user.data1 = user_data;
    return SDL_PushEvent(&e);
}

static int retrive_frame(MediaDecodeContext* ctx, AVFrame** ps)
{
    if (!ctx->frame_avalible)
        return AVERROR(EAGAIN);
    
    *ps = av_frame_clone(ctx->pFrame);
    ctx->frame_avalible = 0;
    return 0;
}

static int stream_decode(MediaDecodeContext* ctx)
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
    }
    else if (ctx->pPacketCache)
    {
        ret = avcodec_send_packet(ctx->pCodecCtx, ctx->pPacketCache);
        if (ret >= 0)
            av_packet_free(&ctx->pPacketCache);
        if (ret == AVERROR(EAGAIN))
            ctx->codec_ready = 1;
    }
    else
    {
        ret = queue_get(&ctx->input_queue, (void**)&ctx->pPacketCache);
    }
    return ret;
}

static int audio_decode_thread(void* arg)
{
    PlayerContext *ctx = (PlayerContext*)arg;
    MediaDecodeContext* pADecodetx = &ctx->audio_decode_ctx;
    AudioSlice *pAudioSlice = NULL;
    double tb = av_q2d(pADecodetx->pStream->time_base);
    init_media_clock(&pADecodetx->clk, 0);
    for(;;)
    {
        PlayerStatus status = read_player_status(ctx);
        if (status == QUIT)
            break;

        if (pAudioSlice)
        {
            int ret = array_queue_put(&ctx->a_play_queue, pAudioSlice);
            if (ret >= 0)
                pAudioSlice = NULL;
            else
            {
                if (ret == AVERROR(EAGAIN))
                    continue;
                throw_event(SDL_QUIT, ctx);
                break;
            }
        }
        
        AVFrame *pFrame = NULL;
        if (retrive_frame(pADecodetx, &pFrame) >= 0)
        {
            int channels = av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO);
            int buf_size = pFrame->nb_samples * channels * sizeof(Uint16);
            int samples_per_channel = pFrame->nb_samples;
            pAudioSlice = create_audio_slice();
            pAudioSlice->data = (Uint16*)av_malloc(buf_size);
            pAudioSlice->len = buf_size;
            pAudioSlice->offset = 0;
            swr_convert(
                    ctx->pASwrCtx, (uint8_t**)&pAudioSlice->data, samples_per_channel, 
                    (const uint8_t**)pFrame->extended_data, pFrame->nb_samples
                );
            pAudioSlice->channel_num = channels;
            pAudioSlice->sample_rate = pADecodetx->pCodecCtx->sample_rate;
            pAudioSlice->pts = pFrame->best_effort_timestamp * tb;
            pAudioSlice->sync_pts = update_media_clock_pts(&pADecodetx->clk, pAudioSlice->pts);
            av_frame_free(&pFrame);
        }
        else
        {
            int ret = stream_decode(pADecodetx);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN))
                    continue;
                throw_event(SDL_QUIT, ctx);
                break;
            }
        }
    }
    if (pAudioSlice)
        free_auido_slice(&pAudioSlice);
    close_array_blocking_queue(&ctx->a_play_queue);
    return 0;
}

static int decode_video_thread(void* arg)
{
    PlayerContext *ctx = (PlayerContext*)arg;
    MediaDecodeContext* pVDecodetx = &ctx->video_decode_ctx;
    VideoPicture *pVideoPic = NULL;
    double tb = av_q2d(ctx->pFormatCtx->streams[ctx->video_stream_id]->time_base);   
    AVRational fps = ctx->pFormatCtx->streams[ctx->video_stream_id]->r_frame_rate;
    double r_fps  = 1.0 / av_q2d(fps);
    init_media_clock(&pVDecodetx->clk, 0);
    for(;;)
    {
        PlayerStatus status = read_player_status(ctx);
        if (status == QUIT)
            break;

        if (pVideoPic)
        {
            int ret = array_queue_put(&ctx->v_play_queue, pVideoPic);
            if (ret >= 0)
                pVideoPic = NULL;
            else
            {
                if (ret == AVERROR(EAGAIN))
                    continue;
                throw_event(SDL_QUIT, ctx);
                break;
            }
        }

        AVFrame *pFrame = NULL;
        if (retrive_frame(pVDecodetx, &pFrame) >= 0)
        {
            pVideoPic = (VideoPicture*)av_mallocz(sizeof(VideoPicture));
            pVideoPic->width = ctx->picture_width;
            pVideoPic->height = ctx->picture_height;
            pVideoPic->pts = pFrame->best_effort_timestamp * tb;
            pVideoPic->duration = r_fps + 0.5 * r_fps * pFrame->repeat_pict;

            // convert pts and duration to sync time
            pVideoPic->sync_pts = update_media_clock_pts(&pVDecodetx->clk, pVideoPic->pts);
            pVideoPic->sync_duration = pVideoPic->duration * pVDecodetx->clk.r_speed;

            int size = av_image_alloc(
                pVideoPic->data, pVideoPic->linesize, pVideoPic->width, pVideoPic->height, ctx->display_pix_fmt, 1
            );
            if (size < 0)
            {
                fprintf(stderr, "Failed to allocate VideoPicture: %s\n", av_err2str(size));
                throw_event(SDL_QUIT, ctx);
                break;
            }
            pVideoPic->size = size;
            sws_scale(
                ctx->sws_ctx, (uint8_t const * const *)pFrame->data, 
                pFrame->linesize, 0, pFrame->height,
                pVideoPic->data, pVideoPic->linesize
            );
            av_frame_free(&pFrame);
        }
        else
        {
            int ret = stream_decode(pVDecodetx);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN))
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

static int update_audio_buf(PlayerContext* ctx)
{
    int ret = 0;
    if (!ctx->pAudioBuf || ctx->pAudioBuf->offset >= ctx->pAudioBuf->len)
    {
        if (ctx->pAudioBuf)
            free_auido_slice(&ctx->pAudioBuf);
        ret = array_queue_get(&ctx->a_play_queue, (void**)&ctx->pAudioBuf, 0);
    }
    return ret;
}

static int fill_audio_data(PlayerContext* ctx, Uint8 *stream, int len, int start_offset)
{
    int written_len = -start_offset;
    while (update_audio_buf(ctx)>=0 && written_len < len)
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
        int real_writting_len = MIN(remain_len, len-written_len);
        memcpy((void*)(stream+written_len), (Uint8*)(p->data) + p->offset, real_writting_len);
        p->offset += real_writting_len;
        written_len += real_writting_len;
    }
    return MAX(0, written_len);
}

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    PlayerContext *ctx = (PlayerContext*)userdata;
    if (update_audio_buf(ctx) < 0)
    {
        memset((void*)stream, 0, len);
        return;
    }

    AudioSlice *p = ctx->pAudioBuf;
    SDL_AudioSpec *spec = &ctx->audio_spec;
    int full_sample_size = spec->channels * (SDL_AUDIO_BITSIZE(spec->format)>>3);
    double bytes_per_second = full_sample_size * spec->freq;
    double audio_sync_pts = p->sync_pts + p->offset / bytes_per_second;
    int offset_compensate = 0;
    int cap = len;
    if (ctx->clock_type != AUDIO_CLOCK)
    {
        double curr_sync_pts = get_sync_clock(&ctx->sync_clock);
        double gap = set_pooling_data(&ctx->audio_diff_pool, audio_sync_pts - curr_sync_pts);
        if (ctx->audio_diff_pool.sz >= ctx->audio_diff_pool.pool_sz)
        {
            double abs_gap = fabs(gap);
            if (abs_gap > SYNC_AUDIO_RESET_THRESHOLD)
                clear_pool(&ctx->audio_diff_pool);
            else if (abs_gap > SYNC_AUDIO_THRESHOLD)
            {
                int compensate_len = abs_gap * bytes_per_second;
                if ((double)compensate_len / len > SYNC_AUDIO_MAX_COMPENSATE)
                    compensate_len = len * SYNC_AUDIO_MAX_COMPENSATE;
                compensate_len = (compensate_len/full_sample_size) * full_sample_size;
                if (gap > 0)
                    cap -= compensate_len;
                else
                    offset_compensate = compensate_len;
            }
        }
    }

    int written_len = fill_audio_data(ctx, stream, cap, offset_compensate);
    if (written_len > 0)
    {
        if (written_len >= full_sample_size)
        {
            int lastest_sample_offset = written_len - full_sample_size;
            while(written_len < len)
            {
                int cpy_len = MIN(full_sample_size, len-written_len);
                memcpy((void*)(stream+written_len), (void*)(stream+lastest_sample_offset), cpy_len);
                written_len += cpy_len;
            }
        }
        if (written_len < len)
            memset((void*)(stream+written_len), 0, len-written_len);
    }
    if (ctx->clock_type == AUDIO_CLOCK)
        set_sync_clock(&ctx->sync_clock, audio_sync_pts);
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
  throw_event(FF_REFRESH_EVENT, opaque);
  return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(PlayerContext *ctx, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, ctx);
}

static double sync_video(VideoPicture** ps, double curr_pts, PlayerContext* ctx, int *show_flag)
{
    *ps = array_queue_peek(&ctx->v_play_queue);
    VideoPicture *p = *ps;
    if (array_queue_size(&ctx->v_play_queue) > 1)
    {
        VideoPicture *next_p = array_queue_next(&ctx->v_play_queue);
        if (next_p)
            p->sync_duration = next_p->sync_pts - p->sync_pts;
    }
    double gap = p->sync_pts - curr_pts;

    total_gap += gap;
    ++frame_num;

    double duration = p->sync_duration;
    double video_sync_r_speed = ctx->video_sync_r_speed;
    if (gap > SYNC_THRESHOLD)
    {
        *show_flag = 0;
        duration = MIN(0.05, gap * 0.5);
        // video_sync_r_speed = 1.0 + (video_sync_r_speed - 1.0) * 0.5;
        ++sync_records[0];
    }
    else if (gap < -SYNC_THRESHOLD)
    {
        // skip current frame
        *show_flag = 0;
        duration = 0;
        array_queue_get(&ctx->v_play_queue, (void**)ps, 0);
        // video_sync_r_speed = 1.0 + (video_sync_r_speed - 1.0) * 0.5;
        ++sync_records[1];
    }
    else
    {
        *show_flag = 1;
        video_sync_r_speed += (gap>0?SYNC_VIDEO_STEP:-SYNC_VIDEO_STEP);
        video_sync_r_speed = MAX(0.0, video_sync_r_speed);
        duration *= video_sync_r_speed;
        array_queue_get(&ctx->v_play_queue, (void**)ps, 0);
        if (video_sync_r_speed <= 1.2 && video_sync_r_speed >= 1.0)
            ++sync_records[2];
        else if (video_sync_r_speed >= 0.8 && video_sync_r_speed < 1.0)
            ++sync_records[3];
        else if (video_sync_r_speed <= 1.4 && video_sync_r_speed > 1.2)
            ++sync_records[4];
        else if (video_sync_r_speed >= 0.6 && video_sync_r_speed < 0.8)
            ++sync_records[5];
        else{++sync_records[6];}
    }
    ctx->video_sync_r_speed = video_sync_r_speed;
    total_video_speed += video_sync_r_speed;
    return duration; 
}

static void display(PlayerContext *ctx)
{
    PlayerStatus status = read_player_status(ctx);
    if (status == QUIT)
        return;

    if (array_queue_size(&ctx->v_play_queue) <= 0)
    {
        if (!ctx->v_play_queue.closed)
            schedule_refresh(ctx, 1);
        return;
    }

    VideoPicture *p = array_queue_peek(&ctx->v_play_queue);
    double curr_pts = get_sync_clock(&ctx->sync_clock);   
    double sys_ts = av_gettime_relative();
    int show_flag = 0;     
    double delay = sync_video(&p, curr_pts, ctx, &show_flag);
    if (show_flag)
    {
        SDL_Rect rect;
        rect.x = (ctx->screen_width - p->width) >> 1;
        rect.y = (ctx->screen_height - p->height) >> 1;
        rect.w = p->width;
        rect.h = p->height;

        SDL_UpdateYUVTexture(
            ctx->texture, &rect, p->data[0], p->linesize[0], 
            p->data[1], p->linesize[1],
            p->data[2], p->linesize[2]
        );
        SDL_RenderClear(ctx->renderer);
        SDL_RenderCopy(ctx->renderer, ctx->texture, NULL, NULL);
        SDL_RenderPresent(ctx->renderer);
    }
    if (p != array_queue_peek(&ctx->v_play_queue))
        free_video_picture(&p);
    delay -= (av_gettime_relative() - sys_ts) / SYSTEM_TIME_BASE;
    schedule_refresh(ctx, MAX(1.0, delay * 1000));
}

static int init_parser(AVFormatContext* pFormatCtx, int stream_id, AVCodec **psCodec, AVCodecContext **psCodecCtx)
{
    // Find the decoder for the audio stream
    AVCodecParameters *codec_par = pFormatCtx->streams[stream_id]->codecpar;
    AVCodec *pCodec = avcodec_find_decoder(codec_par->codec_id);
    if(pCodec==NULL) {
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

static int open_audio_stream(PlayerContext* ctx, int stream_id)
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

    SDL_AudioSpec   wanted_spec;
    wanted_spec.freq = pACtx->pCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 2;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
    wanted_spec.callback = audio_callback;
    wanted_spec.userdata = ctx;
    if(SDL_OpenAudio(&wanted_spec, &ctx->audio_spec) < 0)
    {
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    // Init audio resample context
    // Convert any audio format to AUDIO_S16SYS
    ctx->pASwrCtx = swr_alloc();
    if (swr_alloc_set_opts(ctx->pASwrCtx, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, pACtx->pCodecCtx->sample_rate,
        pACtx->pCodecCtx->channel_layout, pACtx->pCodecCtx->sample_fmt, pACtx->pCodecCtx->sample_rate, 0, NULL) < 0)
    {
        fprintf(stderr, "Couldn't set audio convert context\n");
        throw_event(SDL_QUIT, ctx);
        return -1; 
    }
    if (swr_init(ctx->pASwrCtx) < 0)
    {
        fprintf(stderr, "Couldn't init audio convert context\n");
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    double coeff = exp(log(0.01)/SYNC_AUDIO_DIFF_POOLING_SIZE);
    reset_pool(&ctx->audio_diff_pool, SYNC_AUDIO_DIFF_POOLING_SIZE, coeff, 0.01);
    ctx->a_decode_tid = SDL_CreateThread(audio_decode_thread, "audio_decoder", ctx);
    return 0;
}

static int open_video_stream(PlayerContext* ctx, int stream_id)
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

    // Init video scale context
    // Convert any video format to YUV420P in [width, height]
    ctx->display_pix_fmt = AV_PIX_FMT_YUV420P;
    AVCodecContext *pVCodecCtx = pVCtx->pCodecCtx;
    int src_w = pVCodecCtx->width;
    int src_h = pVCodecCtx->height;
    double ratio = (double)src_w / src_h;
    int dst_h = ctx->screen_height;
    int dst_w = dst_h * ratio;
    if (dst_w > ctx->screen_width)
    {
        dst_h = ctx->screen_width / ratio;
        dst_w = ctx->screen_width;
    }
    ctx->picture_height = dst_h;
    ctx->picture_width = dst_w;
    ctx->sws_ctx = sws_getContext(
        src_w, src_h, pVCtx->pCodecCtx->pix_fmt,
        dst_w, dst_h, ctx->display_pix_fmt,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    ctx->v_decode_tid = SDL_CreateThread(decode_video_thread, "video_decoder", ctx);
    return 0;
}

static int open_stream_component(PlayerContext* ctx, enum AVMediaType media_type)
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
    PlayerContext *ctx = (PlayerContext*)arg;

    // Open video file
    if(avformat_open_input(&ctx->pFormatCtx, ctx->filename, NULL, NULL)!=0)
    {
        fprintf(stderr, "Could not open %s\n", ctx->filename);
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    // Retrieve stream information
    if(avformat_find_stream_info(ctx->pFormatCtx, NULL)<0)
    {
        fprintf(stderr, "Could not find stream info in %s\n", ctx->filename);
        throw_event(SDL_QUIT, ctx);
        return -1;
    }

    // Dump information about file onto standard error
    av_dump_format(ctx->pFormatCtx, 0, ctx->filename, 0);
    
    int ret = 0;
    // Open Audio Stream if exist
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

    AVPacket *p = NULL; 
    ret = 0;   
    MediaDecodeContext *pACtx = &ctx->audio_decode_ctx;
    MediaDecodeContext *pVCtx = &ctx->video_decode_ctx;
    for(;;)
    {
        PlayerStatus status = read_player_status(ctx);
        if (status == QUIT)
            break;

        if (status == PAUSE)
            continue;

        p = av_packet_alloc();
        ret = av_read_frame(ctx->pFormatCtx, p);
        if (ret < 0)
        {
            if (ret != AVERROR_EOF)
            {
                fprintf(stderr, "Demux Error: %s\n", av_err2str(ret));
                throw_event(SDL_QUIT, ctx);
            }
            break;
        }
        
        if (p && p->stream_index == ctx->audio_stream_id)
        {
            ret = queue_put(&pACtx->input_queue, p, p->size);
            if (ret < 0)
                break;  
            p = NULL;
        }
            
        if (p && p->stream_index == ctx->video_stream_id)
        {
            ret = queue_put(&pVCtx->input_queue, p, p->size);
            if (ret < 0)
                break;  
            p = NULL;
        }

        if (p)
            av_packet_free(&p);
    }
    // Flush Decoder
    if (ret == AVERROR_EOF)
    {
        queue_put(&pACtx->input_queue, NULL, 0);
        queue_put(&pVCtx->input_queue, NULL, 0);
    }
    av_packet_free(&p);

    // close blocking queue to prevent consumer thread wait forever
    close_blocking_queue(&pACtx->input_queue);
    close_blocking_queue(&pVCtx->input_queue);

    // close Format Context
    avformat_close_input(&ctx->pFormatCtx);
    return 0;
}

static void free_player_context(PlayerContext** ctx_s)
{
    if (ctx_s == NULL || *ctx_s == NULL)
        return;

    PlayerContext *ctx = *ctx_s;
    SDL_LockMutex(ctx->mutex);
    ctx->status = QUIT;
    SDL_UnlockMutex(ctx->mutex);

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

    free_array_blocking_queue(&ctx->a_play_queue, (Destructor)free_auido_slice);
    free_array_blocking_queue(&ctx->v_play_queue, (Destructor)free_video_picture);

    if (ctx->sws_ctx)
        sws_freeContext(ctx->sws_ctx);

    if (ctx->pASwrCtx)
    {
        swr_close(ctx->pASwrCtx);
        swr_free(&ctx->pASwrCtx);
    }
        
    free_auido_slice(&ctx->pAudioBuf);
    close_media_decode_context(&ctx->audio_decode_ctx);
    close_media_decode_context(&ctx->video_decode_ctx);
    avformat_close_input(&ctx->pFormatCtx);

    *ctx_s = NULL;
}

static void play(PlayerContext* ctx)
{
    if (!ctx)
        return;

    ctx->demux_tid = SDL_CreateThread(demux_thread, "demuxer", ctx);
    ctx->status = Play;
    init_sync_clock(&ctx->sync_clock, 0);
    if (ctx->audio_stream_id >= 0)
        while(array_queue_size(&ctx->a_play_queue) == 0)
            SDL_Delay(1);
        SDL_PauseAudio(0);
    while(array_queue_size(&ctx->v_play_queue) == 0)
        SDL_Delay(1);
    schedule_refresh(ctx, 1);
}

static int init_sdl(PlayerContext* ctx)
{
    // Init SDL2
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);

    // Set SDL2 Parameters to show video frame
    char window_name[1024];
    sprintf(window_name, "SimplePlayer - %s", ctx->filename);
    ctx->screen = SDL_CreateWindow(
                window_name,
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                ctx->screen_width,
                ctx->screen_height,
                0
            );
    if (!ctx->screen)
    {
        fprintf(stderr, "Failed to create SDL2 Window\n");
        return -1;
    }

    ctx->renderer = SDL_CreateRenderer(ctx->screen, -1, SDL_RENDERER_ACCELERATED);
    if (!ctx->renderer) {
        fprintf(stderr, "SDL: could not create renderer: %s\n", SDL_GetError());
        return -1;
    }

    ctx->texture = SDL_CreateTexture(
            ctx->renderer,
            SDL_PIXELFORMAT_YV12,
            SDL_TEXTUREACCESS_STREAMING,
            ctx->screen_width,
            ctx->screen_height
        );
    if (!ctx->texture) {
        fprintf(stderr, "SDL: could not create texture: %s\n", SDL_GetError());
        return -1;
    }
    return 0;
}

static int init_player_context(PlayerContext* ctx)
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

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "You should input a video file path\n");
        return -1;
    }

    PlayerContext *ctx = av_mallocz(sizeof(PlayerContext));
    strcpy(ctx->filename, argv[1]);
    ctx->screen_width = 1280;
    ctx->screen_height = 720;
    ctx->clock_type = AUDIO_CLOCK;

    if (init_player_context(ctx) < 0)
        return -1;

    play(ctx);

    SDL_Event event;
    for(;;)
    {
        SDL_WaitEvent(&event);
        switch(event.type) {
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            free_player_context(&ctx);
            for (int i=0;i<7;++i)
                fprintf(stderr, "%d\n", sync_records[i]);
            fprintf(
                stderr, "avg gap: %fs  avg video speed: %f  frame num: %d\n", 
                total_gap/frame_num, 
                total_video_speed/frame_num, 
                frame_num
                );
            return 0;
            break;
        case FF_REFRESH_EVENT:
            display(event.user.data1);
            break;
        default:
            break;
        }
    }
    free_player_context(&ctx);

    for (int i=0;i<7;++i)
        fprintf(stderr, "%d\n", sync_records[i]);

    return 0;
}
