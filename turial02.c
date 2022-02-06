#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <SDL2/SDL.h>

// Initalizing these to NULL prevents segfaults!
AVFormatContext   *pFormatCtx = NULL;
int               i, videoStream;
AVCodecContext    *pCodecCtx = NULL;
AVCodec           *pCodec = NULL;
AVFrame           *pFrame = NULL;
uint8_t           *rgb_data[4] = {NULL};
int               rgb_data_linesize[4];
AVPacket          *packet = NULL;
int               frameFinished;
int               numBytes;

SDL_Event event;
SDL_Window *screen = NULL;
SDL_Renderer *renderer = NULL;
SDL_Texture *texture = NULL;

int quit(int code) {
    if (!packet)
        av_packet_free(&packet);

    if (!rgb_data[0])
        av_freep(&rgb_data[0]);

    if (!pFrame)
        av_frame_free(&pFrame);

    if (!pCodecCtx)
    {
        avcodec_close(pCodecCtx);
        avcodec_free_context(&pCodecCtx);
    }

    if (!pFormatCtx)
    {  
        avformat_close_input(&pFormatCtx);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(screen);
    SDL_Quit();
    
    return code;
}


int main(int argc, char *argv[]) {
    if(argc < 2) {
        printf("Please provide a movie file\n");
        return -1;
    }

    // Init SDL2
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER);

    
    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        return quit(-1); // Couldn't open file
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return quit(-1); // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);
    
    // Find the first video stream
    // New API
    if(videoStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0) < 0)
    {
        fprintf(stderr, "Could not find %s stream", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
        return quit(-1);
    }

    // Find the decoder for the video stream
    AVCodecParameters *codec_par = pFormatCtx->streams[videoStream]->codecpar;
    pCodec = avcodec_find_decoder(codec_par->codec_id);
    if(pCodec==NULL) {
        fprintf(stderr, "Unsupported codec!\n");
        return quit(-1); // Codec not found
    }
    // Alloc Codec Context
    pCodecCtx = avcodec_alloc_context3(pCodec);

    // Copy context
    if (avcodec_parameters_to_context(pCodecCtx, codec_par) < 0)
    {
        fprintf(stderr, "Couldn't copy codec context");
        return quit(-1); // Error copying codec context
    }

    // Open codec
    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
        return quit(-1); // Could not open codec
    
    // Allocate video frame
    pFrame=av_frame_alloc();

    // Allocate rgb data buffer
    if(av_image_alloc(rgb_data, rgb_data_linesize, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24, 1) < 0)
    {
        fprintf(stderr, "Could not allocate rgb data buffer\n");
        return quit(-1);
    }

    // Set SDL2 Parameters to show video frame
    screen = SDL_CreateWindow(
                "ffmpeg4-tutorial02",
                SDL_WINDOWPOS_UNDEFINED,
                SDL_WINDOWPOS_UNDEFINED,
                pCodecCtx->width,
                pCodecCtx->height,
                0
            );
    if (!screen) {
        fprintf(stderr, "SDL: could not create window - exiting\n");
        return quit(-1);
    }

    renderer = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        fprintf(stderr, "SDL: could not create renderer - exiting\n");
        return quit(-1);
    }

    texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_YV12,
            SDL_TEXTUREACCESS_STREAMING,
            pCodecCtx->width,
            pCodecCtx->height
        );
    if (!texture) {
        fprintf(stderr, "SDL: could not create texture - exiting\n");
        return quit(-1);
    }

    SDL_Rect rect;
    rect.x = 0;
	rect.y = 0;
	rect.w = pCodecCtx->width;
	rect.h = pCodecCtx->height;

    int y_pitch = pCodecCtx->width;
    int uv_pitch = pCodecCtx->width >> 1;

    // Read frames and save first five frames to disk
    packet = av_packet_alloc();
    i=0;
    int stream_flag = 0;
    while(stream_flag >= 0 && i < 100) {
        stream_flag = av_read_frame(pFormatCtx, packet);
        // Flush the decoder when stream_flag < 0 (Usually for no more data)
        AVPacket *pkt = stream_flag>=0?packet:NULL;
        // Is this a packet from the video stream?
        if(!pkt || packet->stream_index==videoStream) {
            // Send Packet to decoder
            int decode_flag = avcodec_send_packet(pCodecCtx, pkt);
            if (decode_flag < 0)
            {
                fprintf(stderr, "Error during decoding (%s)\n", av_err2str(decode_flag));
                break;
            }
            // Decode frames as much as possible
            while(decode_flag >= 0)
            {
                decode_flag = avcodec_receive_frame(pCodecCtx, pFrame);
                if (decode_flag < 0) {
                    // those two return values are special and mean there is no output
                    // frame available, but there were no errors during decoding
                    if (decode_flag == AVERROR_EOF || decode_flag == AVERROR(EAGAIN))
                        break;
        
                    fprintf(stderr, "Error during decoding (%s)\n", av_err2str(decode_flag));
                    stream_flag = -1;
                    break;
                }
                SDL_UpdateYUVTexture(
                    texture, &rect, 
                    pFrame->buf[0]->data, y_pitch,
                    pFrame->buf[1]->data, uv_pitch,
                    pFrame->buf[2]->data, uv_pitch
                );
                SDL_RenderClear(renderer);
                SDL_RenderCopy(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);

                av_frame_unref(pFrame);
            }
        }
        av_packet_unref(packet);
        SDL_PollEvent(&event);
        switch(event.type) {
            case SDL_QUIT:
                break;
            default:
                break;
        }
    }
    
    return quit(0);
}

