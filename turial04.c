#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>

#define SDL_AUDIO_BUFFER_SIZE 1024
#define QUEUE_SIZE 64

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

