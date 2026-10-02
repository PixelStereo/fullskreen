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
echo "media created in $(pwd)"
