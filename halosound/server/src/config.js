module.exports = {
    HTTP_PORT: 8080,
    WS_PORT: 8081,
    AUDIO_SAMPLE_RATE: 48000,
    AUDIO_BLOCK_SIZE: 256,
    VIDEO_EXTENSIONS: ['.mkv', '.mp4', '.avi', '.m4v', '.ts', '.webm'],
    FFMPEG_PATH: process.env.FFMPEG_PATH || 'F:/hrtf/ffmpeg-build/bin/ffmpeg.exe',
    FFPROBE_PATH: process.env.FFPROBE_PATH || 'F:/hrtf/ffmpeg-build/bin/ffprobe.exe'
};
