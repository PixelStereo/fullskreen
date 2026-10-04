#!/bin/sh
# Test media (requires the ffmpeg program)
cd "$(dirname "$0")" && mkdir -p media && cd media
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 4 -pix_fmt yuv420p -c:v libx264 h264.mp4
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=1920x1080:rate=25 -t 2 -c:v prores_ks -profile:v 2 prores.mov
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 -c:v hap hap.mov
ffmpeg -loglevel error -y -f lavfi -i "smptebars=size=1024x768" -frames:v 1 bars.png
# Sound: 440 Hz then 880 Hz from 2 s (44.1 kHz mono: exercises resampling), and a video with an audio track (660 Hz)
ffmpeg -loglevel error -y -f lavfi -i "aevalsrc=0.5*sin(2*PI*if(lt(t\,2)\,440\,880)*t):s=44100:d=4" tone.wav
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=4" \
  -t 4 -pix_fmt yuv420p -c:v libx264 -c:a aac -b:a 128k -shortest av.mp4
# Frame index coded in the luminance (Y = 20 + 4 × frame number), 25 fps, 2 s, GOP of 25: ping-pong test
ffmpeg -loglevel error -y -f lavfi -i "color=black:s=64x64:r=25:d=2,format=yuv420p,geq=lum='20+N*4':cb=128:cr=128" \
  -c:v libx264 -g 25 -bf 2 -pix_fmt yuv420p index.mp4
# Pixel formats: a smooth picture in each layout the GPU converts, and two it does not (converted on the CPU).
# Odd sizes where the format allows them (chroma planes rounded up).
G="gradients=size=322x182:c0=0xff4020:c1=0x2060ff:c2=0x40ff80:c3=0xffff40:nb_colors=4:x0=0:y0=0:x1=322:y1=182:seed=7,trim=end_frame=1,loop=loop=2:size=1:start=0"
GA="$G,format=rgba,geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='40+200*X/W'"
ffmpeg -loglevel error -y -f lavfi -i "$G" -frames:v 2 -pix_fmt yuv420p -c:v libx264 -crf 10 fmt_yuv420p.mp4
ffmpeg -loglevel error -y -f lavfi -i "$G" -frames:v 2 -vf scale=321:181 -pix_fmt yuv420p -c:v rawvideo fmt_yuv420p_odd.nut
ffmpeg -loglevel error -y -f lavfi -i "$G" -frames:v 2 -pix_fmt yuvj420p -c:v mjpeg -q:v 2 fmt_yuvj420p.mov
ffmpeg -loglevel error -y -f lavfi -i "$G" -frames:v 2 -c:v prores_ks -profile:v 2 -pix_fmt yuv422p10le fmt_yuv422p10.mov
ffmpeg -loglevel error -y -f lavfi -i "$GA" -frames:v 2 -c:v prores_ks -profile:v 4 -pix_fmt yuva444p10le fmt_yuva444p10.mov
for f in nv12 yuv444p gbrp10le gray gray16le bgr0 rgb48le yuyv422 rgb565le; do
  ffmpeg -loglevel error -y -f lavfi -i "$G" -frames:v 2 -pix_fmt $f -c:v rawvideo fmt_$f.nut
done
ffmpeg -loglevel error -y -f lavfi -i "$GA" -frames:v 2 -pix_fmt rgba -c:v png fmt_rgba.mov
echo "media created in $(pwd)"
