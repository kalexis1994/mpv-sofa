#!/bin/bash
cd /f/hrtf/ffmpeg-src
rm -f decoder_diag.txt rematrix_overflow.txt pack_overflow.txt
cp libavcodec/avcodec-62.dll .
./ffmpeg.exe -ss 00:09:10 -t 40 -i "/f/Saving Private Ryan (1998).mkv" -map 0:1 -c:a pcm_s32le -f wav /f/hrtf/build/bin/spr_diag.wav -y 2>&1
echo "EXIT CODE: $?"
ls -la decoder_diag.txt rematrix_overflow.txt pack_overflow.txt /f/hrtf/build/bin/spr_diag.wav 2>&1
