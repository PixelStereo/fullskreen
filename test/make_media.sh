#!/bin/sh
# Médias de test (nécessite le programme ffmpeg)
cd "$(dirname "$0")" && mkdir -p media && cd media
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 4 -pix_fmt yuv420p -c:v libx264 h264.mp4
ffmpeg -loglevel error -y -f lavfi -i testsrc=size=1920x1080:rate=25 -t 2 -c:v prores_ks -profile:v 2 prores.mov
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=1280x720:rate=30 -t 2 -c:v hap hap.mov
ffmpeg -loglevel error -y -f lavfi -i "smptebars=size=1024x768" -frames:v 1 bars.png
echo "médias créés dans $(pwd)"
