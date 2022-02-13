#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <SDL2/SDL.h>

#define SDL_AUDIO_BUFFER_SIZE 1024

// Initalizing these to NULL prevents segfaults!
AVFormatContext   *pFormatCtx = NULL;
int               i, audioStream;
AVCodecContext    *pCodecCtx = NULL;
AVCodec           *pCodec = NULL;
AVFrame           *pFrame = NULL;
AVPacket          *packet = NULL;
int               frameFinished;
int               numBytes;
uint8_t           *out_buf = NULL;

SDL_Event event;
SDL_AudioSpec   wanted_spec, spec;

int quit(int code) {
    if (packet)
        av_packet_free(&packet);

    if (pFrame)
        av_frame_free(&pFrame);

    if (pCodecCtx)
    {
        avcodec_close(pCodecCtx);
        avcodec_free_context(&pCodecCtx);
    }

    if (pFormatCtx)
    {  
        avformat_close_input(&pFormatCtx);
    }

    if (out_buf)
    {
        free(out_buf);
    }

    SDL_Quit();
    
    return code;
}


int main(int argc, char *argv[]) {
    if(argc < 2) {
        printf("Please provide a movie file\n");
        return -1;
    }
    
    // Open video file
    if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
        return quit(-1); // Couldn't open file
    
    // Retrieve stream information
    if(avformat_find_stream_info(pFormatCtx, NULL)<0)
        return quit(-1); // Couldn't find stream information
    
    // Dump information about file onto standard error
    av_dump_format(pFormatCtx, 0, argv[1], 0);
    
    // Find the audio stream
    // New API
    audioStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if(audioStream < 0)
    {
        fprintf(stderr, "Could not find %s stream", av_get_media_type_string(AVMEDIA_TYPE_AUDIO));
        return quit(-1);
    }

    // Find the decoder for the audio stream
    AVCodecParameters *codec_par = pFormatCtx->streams[audioStream]->codecpar;
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
    
    // Allocate audio frame
    pFrame=av_frame_alloc();

    // Init Audio Convert Context
    // Convert any audio format to AUDIO_S16SYS
    struct SwrContext *pAudioConvertCtx = swr_alloc();
    if (swr_alloc_set_opts(pAudioConvertCtx, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, pCodecCtx->sample_rate,
        pCodecCtx->channel_layout, pCodecCtx->sample_fmt, pCodecCtx->sample_rate, 0, NULL) < 0)
        {
            fprintf(stderr, "Couldn't set audio convert context");
            return quit(-1); // Error copying codec context
        }
    if (swr_init(pAudioConvertCtx) < 0)
    {
        fprintf(stderr, "Couldn't init audio convert context");
        return quit(-1); // Error copying codec context
    }
    int line_size;
    int output_buf_size = av_samples_get_buffer_size(&line_size, 2, 1024, AV_SAMPLE_FMT_S16, 0);
    out_buf = (uint8_t*)malloc(output_buf_size);

    // Set SDL2 Parameters to play audio
    // Init SDL2
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER);
    // Set audio settings from codec info
    wanted_spec.freq = pCodecCtx->sample_rate;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 2;
    wanted_spec.silence = 0;
    wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;

    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &spec, SDL_AUDIO_ALLOW_ANY_CHANGE);
    if (audio_device == 0)
    {
        fprintf(stderr, "Couldn't open audio device");
        return quit(-1); // Error copying codec context
    }

    // Read frames and save first five frames to disk
    packet = av_packet_alloc();
    int stream_flag = 0;
    while(stream_flag >= 0) {
        stream_flag = av_read_frame(pFormatCtx, packet);
        // Flush the decoder when stream_flag < 0 (Usually for no more data)
        AVPacket *pkt = stream_flag>=0?packet:NULL;
        // Is this a packet from the video stream?
        if(!pkt || packet->stream_index==audioStream) {
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
                int data_size = (pFrame->nb_samples * pFrame->channels) << 1;
                if (data_size > output_buf_size)
                {
                    output_buf_size = data_size;
                    free(out_buf);
                    out_buf = (uint8_t*)malloc(output_buf_size);
                }

                swr_convert(
                    pAudioConvertCtx, &out_buf, output_buf_size>>1, (const uint8_t **)pFrame->data, pFrame->nb_samples
                );

                SDL_QueueAudio(
                    audio_device, out_buf, data_size
                );
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
    double duration = (double) pFormatCtx->duration / AV_TIME_BASE;
    SDL_PauseAudioDevice(audio_device, 0);
    SDL_Delay((uint32_t)(duration*1000));
    return quit(0);
}

