@echo off
rem HaloSound server launcher
rem Usage: start-server.cmd [carpeta-de-medios]  (default: C:\Peliculas)

set FFMPEG_PATH=C:\Users\kalex\hrtf-build\ffmpeg-build\bin\ffmpeg.exe
set FFPROBE_PATH=C:\Users\kalex\hrtf-build\ffmpeg-build\bin\ffprobe.exe

set MEDIA_DIR=%~1
if "%MEDIA_DIR%"=="" set MEDIA_DIR=C:\Peliculas

node "%~dp0server\src\index.js" "%MEDIA_DIR%" --verbose
