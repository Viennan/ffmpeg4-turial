# ffmpeg4-turial
## Turial01 - 05
FFmpeg 4.0+ turial base on http://dranger.com/ffmpeg/ with sdl2</br>
## PurePlayer
A simple video player with the same archiecture as ffplay</br>
There are three mainly distinctive characteristics comparing with ffplay</br>
1. lock-free clock
2. synchronization strategy of audio and video based on scaled pts
3. stepless playing speed control based on 1. and 2.</br>
   you can real-timely and frame-lossly change playing speed with any step length in theory.</br>
   In practice, we set change step to 0.05 and speed range to 0.5-2.0<br>
## VCutExample
Accurately cut video file with minimum decoding/encoding process. </br>
VCutExample will only take decoding/encoding at head and tail gop of the destination time range. The rest data will be simply copyed.</br>
For accurcy, it will decode at least one frame. If file has audio streams, cutting accurcy will be at audio frame level. If video only, it will reach stream time base level.</br>
At present, it has only been tested on H.264 video stream and aac audio stream.
