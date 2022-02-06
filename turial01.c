#include <stdio.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>


void SaveFrame(uint8_t *data, int linesize, int width, int height, int iFrame) {
  FILE *pFile;
  char szFilename[32];
  int  y;
  
  // Open file
  sprintf(szFilename, "./output00/frame%d.ppm", iFrame);
  pFile=fopen(szFilename, "wb");
  if(pFile==NULL)
    return;
  
  // Write header
  fprintf(pFile, "P6\n%d %d\n255\n", width, height);
  
  // Write pixel data
  fwrite(data, 1, linesize * height, pFile);
  
  // Close file
  fclose(pFile);
}

int main(int argc, char *argv[]) {
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
  struct SwsContext *sws_ctx = NULL;

  if(argc < 2) {
    printf("Please provide a movie file\n");
    return -1;
  }
  // Register all formats and codecs
  // No need to call this function when ffmpeg is upper than 4.2
  // av_register_all();
  
  // Open video file
  if(avformat_open_input(&pFormatCtx, argv[1], NULL, NULL)!=0)
    return -1; // Couldn't open file
  
  // Retrieve stream information
  if(avformat_find_stream_info(pFormatCtx, NULL)<0)
    return -1; // Couldn't find stream information
  
  // Dump information about file onto standard error
  av_dump_format(pFormatCtx, 0, argv[1], 0);
  
  // Find the first video stream
  // New API
  if(videoStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0) < 0)
  {
    fprintf(stderr, "Could not find %s stream", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
    return -1;
  }

  // Find the decoder for the video stream
  AVCodecParameters *codec_par = pFormatCtx->streams[videoStream]->codecpar;
  pCodec = avcodec_find_decoder(codec_par->codec_id);
  if(pCodec==NULL) {
    fprintf(stderr, "Unsupported codec!\n");
    return -1; // Codec not found
  }
  // Alloc Codec Context
  pCodecCtx = avcodec_alloc_context3(pCodec);

  // Copy context
  if (avcodec_parameters_to_context(pCodecCtx, codec_par) < 0)
  {
    fprintf(stderr, "Couldn't copy codec context");
    return -1; // Error copying codec context
  }

  // Open codec
  if(avcodec_open2(pCodecCtx, pCodec, NULL)<0)
    return -1; // Could not open codec
  
  // Allocate video frame
  pFrame=av_frame_alloc();

  // Allocate rgb data buffer
  if(av_image_alloc(rgb_data, rgb_data_linesize, pCodecCtx->width, pCodecCtx->height, AV_PIX_FMT_RGB24, 1) < 0)
  {
    fprintf(stderr, "Could not allocate rgb data buffer\n");
    return -1;
  }

  // initialize SWS context for software scaling
  sws_ctx = sws_getContext(pCodecCtx->width,
			   pCodecCtx->height,
			   pCodecCtx->pix_fmt,
			   pCodecCtx->width,
			   pCodecCtx->height,
			   AV_PIX_FMT_RGB24,
			   SWS_BILINEAR,
			   NULL,
			   NULL,
			   NULL
			   );

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
        // Convert the image from its native format to RGB
        sws_scale(sws_ctx, (uint8_t const * const *)pFrame->data,
            pFrame->linesize, 0, pCodecCtx->height,
            rgb_data, rgb_data_linesize);
        // Save the frame to disk
        SaveFrame(rgb_data[0], rgb_data_linesize[0], pCodecCtx->width, pCodecCtx->height, ++i);
        
        av_frame_unref(pFrame);
      }
    }
    
    av_packet_unref(packet);
  }

  // Free the Packet
  av_packet_free(&packet);
  
  // Free the RGB image
  av_freep(&rgb_data[0]);
  
  // Free the YUV frame
  av_frame_free(&pFrame);
  
  // Close the codecs
  avcodec_close(pCodecCtx);

  // Close the video file
  avformat_close_input(&pFormatCtx);
  
  return 0;
}