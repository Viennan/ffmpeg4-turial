#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define QUEUE_SIZE 64

#define FF_REFRESH_EVENT (SDL_USEREVENT)
#define FF_QUIT_EVENT (SDL_USEREVENT + 1)

#define QUEUE_INDEX(x) ((x) & (QUEUE_SIZE-1))

typedef struct BlockingQueue {
    void *ptrs[QUEUE_SIZE];
    int r_id, w_id;
    SDL_mutex       *mutex;
    SDL_cond        *cond;
} BlockingQueue;

typedef struct PlayerContext {
    const char*             filename;

    AVFormatContext         *pFormatCtx;
    int                     audio_stream_id, video_stream_id;
    AVCodecContext          *pACodecCtx, *pVCodecCtx;
    AVCodec                 *pACodec, *pVCodec;
    BlockingQueue           a_queue, v_queue;
    SDL_Thread              *demux_tid, *v_decode_tid;

    int                     screen_width, screen_height;
    SDL_Event               event;
    SDL_Window              *screen;
    SDL_Renderer            *renderer;
    SDL_Texture             *texture;

    int                     status;
} PlayerContext;

int blocking_queue_put(BlockingQueue* q, void* dptr)
{
    SDL_LockMutex(q->mutex);
    if (QUEUE_INDEX(q->w_id+1) == q->r_id)
    {
        SDL_UNLockM(q->mutex);
        return -1;
    }
    q->ptrs[q->w_id] = dptr;
    q->w_id = QUEUE_INDEX(q->w_id+1);
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

void* blocking_queue_get(BlockingQueue* q)
{
    void* ptr=NULL;
    SDL_LockMutex(q->mutex);
    while (q->w_id == q->r_id)
        SDL_CondWait(q->cond, q->mutex);
    ptr = q->ptrs[q->r_id];
    q->ptrs[q->r_id] = NULL;
    q->r_id = QUEUE_INDEX(q->r_id+1);
    SDL_UnlockMutex(q->mutex);
    return ptr;
}

int throw_event(Uint32 event_type, void* user_data)
{
    SDL_Event e;
    e.type = event_type;
    e.user.data1 = user_data;
    return SDL_PushEvent(&e);
}

int init_parser(AVFormatContext* pFormatCtx, int stream_id, AVCodec **psCodec, AVCodecContext **psCodecCtx)
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
        fprintf(stderr, "Could not init context of codec %s", pCodec->long_name);
        return -1;
    }
    *psCodec = pCodec;
    *psCodecCtx = pCodecCtx;
    return 0;
}

int init_player_context(PlayerContext *ctx)
{
    // Open video file
    if(avformat_open_input(&ctx->pFormatCtx, ctx->filename, NULL, NULL)!=0)
    {
        fprintf(stderr, "Could not open %s", ctx->filename);
        return -1; 
    }

    // Retrieve stream information
    if(avformat_find_stream_info(ctx->pFormatCtx, NULL)<0)
    {
        fprintf(stderr, "Could not find stream info in %s", ctx->filename);
        return -1;
    }

    // Dump information about file onto standard error
    av_dump_format(ctx->pFormatCtx, 0, ctx->filename, 0);
    
    // Find Audio Stream
    ctx->audio_stream_id = av_find_best_stream(ctx->pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ctx->audio_stream_id < 0)
    {
        fprintf(stderr, "Could not find audio stream in %s", ctx->filename);
        return -1;
    }

    // Find Video Stream
    ctx->video_stream_id = av_find_best_stream(ctx->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (ctx->video_stream_id < 0)
    {
        fprintf(stderr, "Could not find video stream in %s", ctx->filename);
        return -1;
    }

    // Init audio decoder
    if (init_parser(ctx->pFormatCtx, ctx->audio_stream_id, &ctx->pACodec, &ctx->pACodecCtx) < 0)
    {
        fprintf(stderr, "Failed init audio decoder");
        return -1;
    }

    // Init video decoder
    if (init_parser(ctx->pFormatCtx, ctx->video_stream_id, &ctx->pVCodec, &ctx->pVCodecCtx) < 0)
    {
        fprintf(stderr, "Failed init video decoder");
        return -1;
    }




}

int demux_thread(void *arg)
{
    PlayerContext *ctx = (PlayerContext*)arg;

    
        
    


    
}