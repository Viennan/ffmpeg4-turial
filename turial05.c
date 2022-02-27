#include <stdio.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <SDL2/SDL.h>
#include <pthread.h>

#define SDL_AUDIO_BUFFER_SIZE 2048
#define MAX_AUDIO_FRAME_SIZE 192000
#define AUDIO_QUEUE_SIZE 5 * 1024 * 1024
#define VIDEO_QUEUE_SIZE 50 * 1024 * 1024
#define VIDEO_PICTURE_QUEUE_SIZE 50 * 1024 * 1024
#define MAX_ARRAY_QUEUE_SIZE 64 // Must be power of 2
#define MAX_CACHED_PICTURE 3 // Pictures to be shown will not exceeded MAX_CACHED_PICTURE

#define SYNC_THRESHOLD_WEAK 50
#define SYNC_THRESHOLD_STRONG 100
#define SYNC_THRESHOLD_SKIP 200
#define SYNC_VIDEO_STEP 0.05
#define SYNC_VIDEO_R_SPEED_MAX 1.25
#define SYNC_VIDEO_R_SPEED_MIN 0.8
#define SYNC_THRESHOLD_VSPEED_RESET 25
#define SYNC_VIDEO_DIFF_POOLING_SIZE 5

// #define QUEUE_INDEX(x) ((x) & (MAX_ARRAY_QUEUE_SIZE-1))
#define QUEUE_INDEX(x, size) ((x) & ((size)-1))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)


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
    double pts; // the lastest presentation time in ms
    double duration;
} VideoPicture;

typedef struct Clock {
    double ts;  // start time point
    double sys_ts;  // system time stamp
    double speed;  // time spending speed in player view
} Clock;

typedef struct AvgPool {
    int pool_sz;
    int sz;
    int r_ind;
    double data[MAX_ARRAY_QUEUE_SIZE];
    double sum;
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

typedef enum PlayerStatus {
    PAUSE = 0,
    Play = 1,
    QUIT = 2
} PlayerStatus;

typedef struct PlayerContext {
    char                    filename[512];

    AVFormatContext         *pFormatCtx;
    int                     audio_stream_id, video_stream_id;
    AVCodecContext          *pACodecCtx, *pVCodecCtx;
    AVCodec                 *pACodec, *pVCodec;
    SwrContext              *pASwrCtx;
    BlockingQueue           a_queue, v_queue;
    ArrayBlockingQueue      v_play_queue;
    int                     frame_interval;
    Uint8                   *audio_buf;

    int                     screen_width, screen_height;
    int                     picture_width, picture_height;
    enum AVPixelFormat      display_pix_fmt;
    struct SwsContext       *sws_ctx;

    SDL_Window              *screen;
    SDL_Renderer            *renderer;
    SDL_Texture             *texture;
    SDL_AudioSpec           audio_spec;
    SDL_Thread              *demux_tid, *v_decode_tid;
    SDL_mutex               *mutex;

    enum ClockType          clock_type;
    Clock                   clk;
    double                  video_sync_r_speed;
    AvgPool                 video_diff_pool;

    PlayerStatus            status;
    pthread_rwlock_t        ctx_lock;
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

static void init_clock(Clock* c)
{
    c->ts = 0;
    c->speed = 1;
    c->sys_ts = av_gettime_relative();
}

static inline double get_current_time(const Clock *c)
{
    return c->ts + (av_gettime_relative()-c->sys_ts) * c->speed;
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

static void reset_pool(AvgPool* pool, int pool_sz)
{
    pool->pool_sz = MIN(pool_sz, MAX_ARRAY_QUEUE_SIZE);
    pool->r_ind = 0;
    pool->sum = 0;
    pool->sz = 0;
    memset(&pool->data[0], 0, sizeof(pool->data));
}

static inline double set_pooling_data(AvgPool* pool, double data)
{
    pool->sz = MIN(pool->pool_sz, pool->sz+1);
    pool->sum += (pool->data[pool->r_ind] - pool->data[QUEUE_INDEX(pool->r_ind+1, MAX_ARRAY_QUEUE_SIZE)]);
    pool->r_ind = QUEUE_INDEX(pool->r_ind+1, MAX_ARRAY_QUEUE_SIZE);
    return pool->sum / MAX(1.0, (double)pool->sz);
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

static void free_blocking_queue(BlockingQueue* q)
{
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->put_cond);
    SDL_DestroyCond(q->get_cond);
    while(q->head != NULL)
    {
        ListNode* node = q->head;
        q->head = q->head->next;
        av_free(node);
    }
    q->tail = NULL;
}

int queue_put_node(BlockingQueue* q, ListNode* node)
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

int queue_put(BlockingQueue* q, void* dptr, uint64_t sz)
{
    ListNode* node = create_list_node(dptr, sz);
    return queue_put_node(q, node);
}

int queue_get(BlockingQueue* q, void **dptr)
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

void init_array_blocking_queue(ArrayBlockingQueue *q, int max_size)
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

void close_array_blocking_queue(ArrayBlockingQueue *q)
{
    SDL_LockMutex(q->mutex);
    q->closed = 1;
    SDL_CondSignal(q->get_cond);
    SDL_CondSignal(q->put_cond);
    SDL_UnlockMutex(q->mutex);
}

void free_array_blocking_queue(ArrayBlockingQueue *q)
{
    SDL_DestroyMutex(q->mutex);
    SDL_DestroyCond(q->put_cond);
    SDL_DestroyCond(q->get_cond);
}

int array_queue_put(ArrayBlockingQueue *q, void *dptr)
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

int array_queue_get(ArrayBlockingQueue *q, void **dptrs, int block)
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

static inline void* array_queue_peek(ArrayBlockingQueue *q)
{
    return q->dptrs[QUEUE_INDEX(q->r_ind+1, MAX_ARRAY_QUEUE_SIZE)];
}

static int read_player_status(PlayerContext* ctx)
{
    SDL_LockMutex(ctx->mutex);
    PlayerStatus status = ctx->status;
    SDL_UnlockMutex(ctx->mutex);
    return status;
}

static double get_master_clock(PlayerContext* ctx)
{
    double ts;
    if (ctx->clock_type == AUDIO_CLOCK)
        ts = get_current_ts(&ctx->clk);
    else
        pthread_rwlock_rdlock(&ctx->ctx_lock);
        ts = get_current_time(&ctx->clk);
        pthread_rwlock_unlock(&ctx->ctx_lock);
    
    return ts;
}

static inline double get_master_clock_speed(PlayerContext* ctx)
{
    return ctx->clk.speed;
}

static inline int throw_event(Uint32 event_type, void* user_data)
{
    SDL_Event e;
    e.type = event_type;
    e.user.data1 = user_data;
    return SDL_PushEvent(&e);
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

static int init_ffmpeg(PlayerContext *ctx)
{
    // Open video file
    if(avformat_open_input(&ctx->pFormatCtx, ctx->filename, NULL, NULL)!=0)
    {
        fprintf(stderr, "Could not open %s\n", ctx->filename);
        return -1; 
    }

    // Retrieve stream information
    if(avformat_find_stream_info(ctx->pFormatCtx, NULL)<0)
    {
        fprintf(stderr, "Could not find stream info in %s\n", ctx->filename);
        return -1;
    }

    // Dump information about file onto standard error
    av_dump_format(ctx->pFormatCtx, 0, ctx->filename, 0);
    
    // Find Audio Stream
    ctx->audio_stream_id = av_find_best_stream(ctx->pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ctx->audio_stream_id < 0)
    {
        fprintf(stderr, "Could not find audio stream in %s\n", ctx->filename);
        return -1;
    }

    // Find Video Stream
    ctx->video_stream_id = av_find_best_stream(ctx->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ctx->video_stream_id < 0)
    {
        fprintf(stderr, "Could not find video stream in %s\n", ctx->filename);
        return -1;
    }

    // Set Each Video Frame's Duration (ms)
    AVStream *pVStream = ctx->pFormatCtx->streams[ctx->video_stream_id];
    int nb_frames = pVStream->nb_frames;
    if (nb_frames)
        ctx->frame_interval = (double)ctx->pFormatCtx->duration / AV_TIME_BASE / nb_frames * 1000;
    else
        ctx->frame_interval = 40;

    // Init audio decoder
    if (init_parser(ctx->pFormatCtx, ctx->audio_stream_id, &ctx->pACodec, &ctx->pACodecCtx) < 0)
    {
        fprintf(stderr, "Failed init audio decoder\n");
        return -1;
    }
    
    // Init audio decode buf
    ctx->audio_buf = av_malloc(MAX_AUDIO_FRAME_SIZE);

    // Init video decoder
    if (init_parser(ctx->pFormatCtx, ctx->video_stream_id, &ctx->pVCodec, &ctx->pVCodecCtx) < 0)
    {
        fprintf(stderr, "Failed init video decoder\n");
        return -1;
    }

    // Init audio resample context
    // Convert any audio format to AUDIO_S16SYS
    ctx->pASwrCtx = swr_alloc();
    if (swr_alloc_set_opts(ctx->pASwrCtx, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, ctx->pACodecCtx->sample_rate,
        ctx->pACodecCtx->channel_layout, ctx->pACodecCtx->sample_fmt, ctx->pACodecCtx->sample_rate, 0, NULL) < 0)
    {
        fprintf(stderr, "Couldn't set audio convert context\n");
        return -1; 
    }
    if (swr_init(ctx->pASwrCtx) < 0)
    {
        fprintf(stderr, "Couldn't init audio convert context\n");
        return -1;
    }

    // Init video scale context
    // Convert any video format to YUV420P in [width, height]
    ctx->display_pix_fmt = AV_PIX_FMT_YUV420P;
    AVCodecContext *pVCodecCtx = ctx->pVCodecCtx;
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
        src_w, src_h, ctx->pVCodecCtx->pix_fmt,
        dst_w, dst_h, ctx->display_pix_fmt,
        SWS_BILINEAR, NULL, NULL, NULL
    );
    return 0;
}

static int demux_thread(void *arg)
{
    PlayerContext *ctx = (PlayerContext*)arg;
    AVPacket *p = NULL; 
    int ret = 0;       
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
            ret = queue_put(&ctx->a_queue, p, p->size);
            if (ret < 0)
                break;  
            p = NULL;
        }
            
        if (p && p->stream_index == ctx->video_stream_id)
        {
            ret = queue_put(&ctx->v_queue, p, p->size);
            if (ret < 0)
                break;  
            p = NULL;
        }
    }
    // Flush Decoder
    if (ret == AVERROR_EOF)
    {
        queue_put(&ctx->a_queue, NULL, 0);
        queue_put(&ctx->v_queue, NULL, 0);
    }
    av_packet_free(&p);

    // close blocking queue to prevent consumer thread wait forever
    close_blocking_queue(&ctx->a_queue);
    close_blocking_queue(&ctx->v_queue);
    return 0;
}

static int retrieve_and_send_packet(BlockingQueue* q, AVCodecContext* pCodecCtx)
{
    AVPacket *pPacket = NULL;
    int ret = queue_get(q, (void**)&pPacket);
    if (ret < 0)
    {
        if (ret != AVERROR(EACCES))
        {
            fprintf(
                stderr, 
                "Error getting a %s packet from queue: (%s)\n", 
                pCodecCtx->codec->long_name, 
                av_err2str(ret)
            );
        }           
    }
    else
    {
        ret = avcodec_send_packet(pCodecCtx, pPacket);
        if (ret < 0)
        {
            fprintf(
                stderr, "Error submitting a %s packet for decoding (%s)\n",
                pCodecCtx->codec->long_name, 
                av_err2str(ret)
            );
        }
        if (!pPacket)
            ret = AVERROR_EOF;  
    }
    av_packet_free(&pPacket);
    return ret;
}

static int decode_video_thread(void* arg)
{
    PlayerContext *ctx = (PlayerContext*)arg;
    VideoPicture *p = NULL;
    AVFrame *pFrame = av_frame_alloc();
    int ready_flag = 0;
    double tb = av_q2d(ctx->pVCodecCtx->time_base);
    AVRational fps = av_guess_frame_rate(ctx->pFormatCtx, ctx->pFormatCtx->streams[ctx->video_stream_id], NULL);
    double r_fps = 1.0 / av_q2d(fps);
    for (;;)
    {
        PlayerStatus status = read_player_status(ctx);
        if (status == QUIT)
            break;

        if (p)
        {
            int ret = array_queue_put(&ctx->v_play_queue, p);
            if (ret < 0)
                break;
            p = NULL;
        }
        else
        {
            if (ready_flag)
            {
                int ret = avcodec_receive_frame(ctx->pVCodecCtx, pFrame);
                if (ret >= 0)
                {
                    p = (VideoPicture*)av_mallocz(sizeof(VideoPicture));
                    p->width = ctx->picture_width;
                    p->height = ctx->picture_height;
                    p->pts = pFrame->best_effort_timestamp * tb;
                    p->duration = tb + 0.5 * r_fps * pFrame->repeat_pict;
                    int size = av_image_alloc(
                        p->data, p->linesize, p->width, p->height, ctx->display_pix_fmt, 1
                    );
                    if (size < 0)
                    {
                        fprintf(stderr, "Failed to allocate VideoPicture: %s\n", av_err2str(size));
                        throw_event(SDL_QUIT, ctx);
                        break;
                    }
                    p->size = size;
                    sws_scale(
                        ctx->sws_ctx, (uint8_t const * const *)pFrame->data, 
                        pFrame->linesize, 0, pFrame->height,
                        p->data, p->linesize
                    );
                    av_frame_unref(pFrame);
                }
                else
                {
                    ready_flag = 0;
                    if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                        continue;
                    fprintf(stderr, "Decoder Error: %s\n", av_err2str(ret));
                    throw_event(SDL_QUIT, ctx);
                    break;
                }
            }
            else
            {
                int ret = retrieve_and_send_packet(&ctx->v_queue, ctx->pVCodecCtx);
                if (ret < 0 && ret != AVERROR_EOF)
                    break;
                ready_flag = 1;
            }
        }
    }  
    av_frame_free(&pFrame);

    if (p)
        free_video_picture(&p);
    
    // close blocking queue to prevent consumer thread wait forever
    close_array_blocking_queue(&ctx->v_play_queue);
    return 0;
}

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    static int offset = 0;
    static int real_size = 0;
    static int avi_len = 0;
    static int ready_flag = 0;
    static int buf_size = MAX_AUDIO_FRAME_SIZE;
    static int eof = 0;

    if (eof)
    {
        memset(stream, 0, len);
        return;
    }

    PlayerContext *ctx = (PlayerContext*)userdata;
    AVFrame *pFrame = av_frame_alloc();
    int input_len = len;
    if (avi_len)
    {
        int cpy_len = avi_len > len ? len: avi_len;
        memcpy(stream, ctx->audio_buf+offset, cpy_len);
        len -= cpy_len;
        avi_len -= cpy_len;
        offset += cpy_len;
        stream += cpy_len;
    }
    
    while (avi_len == 0)
    {
        if (ready_flag)
        {
            int ret = avcodec_receive_frame(ctx->pACodecCtx, pFrame);
            if (ret < 0)
            {
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    ready_flag = 0;
                    continue;
                }
                fprintf(stderr, "Decode Audio Error: %s\n", av_err2str(ret));
                ready_flag = 0;
                throw_event(SDL_QUIT, ctx);
                break;
            }    
            else
            {
                offset = 0;
                real_size = (pFrame->nb_samples * av_get_channel_layout_nb_channels(AV_CH_LAYOUT_STEREO)) << 1;
                avi_len = real_size;
                if (real_size > buf_size)
                {
                    av_free(ctx->audio_buf);
                    ctx->audio_buf = av_malloc(real_size);
                    buf_size = real_size;
                }
                swr_convert(
                    ctx->pASwrCtx, &ctx->audio_buf, buf_size>>2, pFrame->extended_data, pFrame->nb_samples
                );
                break;
            }
        }
        else
        {
            int ret = retrieve_and_send_packet(&ctx->a_queue, ctx->pACodecCtx);
            if (ret < 0)
            {
                if (ret == AVERROR_EOF)
                    eof = 1;
                break;
            }
            ready_flag = 1;
        }
    }

    if (avi_len && len)
    {
        int cpy_len = avi_len > len ? len: avi_len;
        memcpy(stream, ctx->audio_buf+offset, cpy_len);
        avi_len -= cpy_len;
        offset += cpy_len;
        len -= cpy_len;
        stream += cpy_len;
    }

    if (len)
        memset(stream, 0, len);

    av_frame_free(&pFrame);
}

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque) {
  throw_event(FF_REFRESH_EVENT, opaque);
  return 0; /* 0 means stop timer */
}

/* schedule a video refresh in 'delay' ms */
static void schedule_refresh(PlayerContext *ctx, int delay) {
  SDL_AddTimer(delay, sdl_refresh_timer_cb, ctx);
}

static double sync_video(VideoPicture** ps, double curr_pts, double r_speed, PlayerContext* ctx, int *show_flag)
{
    *ps = array_queue_peek(&ctx->v_play_queue);
    VideoPicture *p = *ps;
    if (array_queue_size(&ctx->v_play_queue) > 1)
    {
        VideoPicture *next_p = array_queue_next(&ctx->v_play_queue);
        if (next_p)
            p->duration = next_p->pts - p->pts;
    }
    double gap = set_pooling_data(&ctx->video_diff_pool, p->pts - curr_pts);
    double duration = p->duration * r_speed;
    double video_sync_r_speed = 1.0;
    if (gap > SYNC_THRESHOLD_SKIP)
    {
        *show_flag = 0;
        duration = gap * r_speed * 0.5;
    }
    else if (gap < -SYNC_THRESHOLD_SKIP)
    {
        *show_flag = 0;
        duration = 0;
    }
    else if (gap > SYNC_THRESHOLD_STRONG)
    {
        *show_flag = 1;
        video_sync_r_speed = SYNC_VIDEO_R_SPEED_MAX;
    }
    else if (gap < -SYNC_THRESHOLD_STRONG)
    {
        *show_flag = 1;
        video_sync_r_speed = SYNC_VIDEO_R_SPEED_MIN;
    }
    else if (gap > SYNC_THRESHOLD_WEAK)
    {
        *show_flag = 1;
        video_sync_r_speed = MIN(SYNC_VIDEO_R_SPEED_MAX, ctx->video_sync_r_speed + SYNC_VIDEO_STEP);
        ctx->video_sync_r_speed = video_sync_r_speed;
    }
    else if (gap < -SYNC_THRESHOLD_WEAK)
    {
        *show_flag = 1;
        video_sync_r_speed = MAX(SYNC_VIDEO_R_SPEED_MIN, ctx->video_sync_r_speed - SYNC_VIDEO_STEP);
        ctx->video_sync_r_speed = video_sync_r_speed;
    }
    else if (fabs(gap) < SYNC_THRESHOLD_VSPEED_RESET)
    {
        *show_flag = 1;
        video_sync_r_speed = 1.0;
    }
    else
    {
        *show_flag = 1;
        video_sync_r_speed = ctx->video_sync_r_speed;
    }
    if (*show_flag)
        array_queue_get(&ctx->v_play_queue, (void**)ps, 0);
    return duration * video_sync_r_speed; 
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
    double r_speed = 1.0 / get_master_clock_speed(ctx);
    double curr_pts = get_master_clock(ctx);   
    int show_flag = 0;     
    double delay = sync_video(&p, curr_pts, r_speed, ctx, &show_flag);
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
        free_video_picture(&p);
    }   
    double lasetest_pts = get_master_clock(ctx);
    delay -= (lasetest_pts-curr_pts);
    schedule_refresh(ctx, MAX(1.0, delay));
}

static int init_sdl(PlayerContext* ctx)
{
    // Set player status to PAUSE
    // The demuxer thread will not start demuxing until the player status setted to Play
    ctx->status = PAUSE;

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

    ctx->mutex = SDL_CreateMutex();
    init_blocking_queue(&ctx->a_queue, AUDIO_QUEUE_SIZE);
    init_blocking_queue(&ctx->v_queue, VIDEO_QUEUE_SIZE);
    init_array_blocking_queue(&ctx->v_play_queue, MAX_CACHED_PICTURE);
    
    if (ctx->audio_stream_id >= 0)
    {
        SDL_AudioSpec   wanted_spec;
        wanted_spec.freq = ctx->pACodecCtx->sample_rate;
        wanted_spec.format = AUDIO_S16SYS;
        wanted_spec.channels = 2;
        wanted_spec.silence = 0;
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;
        wanted_spec.callback = audio_callback;
        wanted_spec.userdata = ctx;
        if(SDL_OpenAudio(&wanted_spec, &ctx->audio_spec) < 0)
        {
            fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());
            return -1;
        }
    }

    ctx->v_decode_tid = SDL_CreateThread(decode_video_thread, "video_decoder", ctx);
    ctx->demux_tid = SDL_CreateThread(demux_thread, "demuxer", ctx);
    
    return 0;
}

static void start_player(PlayerContext* ctx)
{
    if (!ctx)
        return;

    pthread_rwlock_wrlock(&ctx->ctx_lock);
    ctx->status = Play;
    pthread_rwlock_unlock(&ctx->ctx_lock);
    if (ctx->audio_stream_id >= 0)
        SDL_PauseAudio(0);
    schedule_refresh(ctx, 1);
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
    close_blocking_queue(&ctx->a_queue);
    close_blocking_queue(&ctx->v_queue);
    close_array_blocking_queue(&ctx->v_play_queue);
    int status;
    SDL_WaitThread(ctx->demux_tid, &status);
    SDL_WaitThread(ctx->v_decode_tid, &status);
    SDL_DestroyMutex(ctx->mutex);

    // free av packets in queue
    ListNode* node = ctx->a_queue.head;
    while(node)
    {
        av_packet_free((AVPacket**)&node->dptr);
        node = node->next;
    } 
    node = ctx->v_queue.head;
    while(node)
    {
        av_packet_free((AVPacket**)&node->dptr);
        node = node->next;
    }

    // free video picture
    int idx = ctx->v_play_queue.r_ind;
    while(QUEUE_INDEX(idx, MAX_ARRAY_QUEUE_SIZE) != ctx->v_play_queue.w_ind)
    {
        free_video_picture(&ctx->v_play_queue.dptrs[idx]);
        idx = QUEUE_INDEX(idx+1, MAX_ARRAY_QUEUE_SIZE);
    }

    free_blocking_queue(&ctx->a_queue);
    free_blocking_queue(&ctx->v_queue);
    free_array_blocking_queue(&ctx->v_play_queue);
    SDL_DestroyTexture(ctx->texture);
    SDL_DestroyRenderer(ctx->renderer);
    SDL_DestroyWindow(ctx->screen);
    SDL_Quit();

    sws_freeContext(ctx->sws_ctx);
    swr_close(ctx->pASwrCtx);
    swr_free(&ctx->pASwrCtx);

    if (ctx->audio_buf)
        av_free(ctx->audio_buf);

    if (ctx->pACodecCtx)
    {
        avcodec_free_context(&ctx->pACodecCtx);
    }

    if (ctx->pVCodecCtx)
    {
        avcodec_free_context(&ctx->pVCodecCtx);
    }

    if (ctx->pFormatCtx)
        avformat_close_input(&ctx->pFormatCtx);

    pthread_rwlock_destroy(&ctx->ctx_lock);

    *ctx_s = NULL;
}

static int init_player_context(PlayerContext* ctx)
{
    if (!ctx)
        return -1;

    int ret = init_ffmpeg(ctx);
    if (ret < 0)
    {
        fprintf(stderr, "Failed Init FFmpeg part of player context\n");
        free_player_context(&ctx);
        return ret;
    }

    ret = init_sdl(ctx);
    if (ret < 0)
    {
        fprintf(stderr, "Failed Init SDL2 part of player context\n");
        free_player_context(&ctx);
        return ret;
    }

    reset_pool(&ctx->video_diff_pool, SYNC_VIDEO_DIFF_POOLING_SIZE);
    ctx->video_sync_r_speed = 1.0;
    ret = pthread_rwlock_init(&ctx->ctx_lock, NULL);
    if (ret < 0)
        return ret;

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

    if (init_player_context(ctx) < 0)
        return -1;

    SDL_Event event;
    start_player(ctx);
    for(;;)
    {
        SDL_WaitEvent(&event);
        switch(event.type) {
        case FF_QUIT_EVENT:
        case SDL_QUIT:
            free_player_context(&ctx);
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
    return 0;
}
