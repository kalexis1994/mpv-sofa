const WebSocket = require('ws');
const { AudioDemuxer } = require('./ffmpeg-demux');
const config = require('./config');

/**
 * Create WebSocket server for streaming audio PCM data.
 *
 * Protocol:
 * Client -> Server (JSON):
 *   {type: "play", fileId: N}     - Start playback
 *   {type: "seek", time: seconds} - Seek to position
 *   {type: "stop"}                - Stop playback
 *
 * Server -> Client (JSON):
 *   {type: "info", channels, sampleRate, layout, duration}
 *   {type: "seek_done", pts}
 *
 * Server -> Client (Binary):
 *   [8 bytes: Float64BE PTS] [channels * 256 * 4 bytes: Float32LE PCM per channel, planar]
 *
 * @param {number} port - WebSocket port
 * @param {Array} files - Scanned file list
 * @param {Object} options - { verbose: boolean }
 * @returns {WebSocket.Server}
 */
function createWsServer(port, files, options = {}) {
    const wss = new WebSocket.Server({ port });

    wss.on('connection', (ws) => {
        if (options.verbose) console.log('[WS] Client connected');

        let demuxer = null;

        ws.on('message', async (msg) => {
            let data;
            try {
                data = JSON.parse(msg.toString());
            } catch (e) {
                return;
            }

            switch (data.type) {
                case 'play': {
                    const file = files.find(f => f.id === data.fileId);
                    if (!file) {
                        ws.send(JSON.stringify({ type: 'error', message: 'File not found' }));
                        return;
                    }

                    /* Clean up previous demuxer */
                    if (demuxer) {
                        demuxer.stop();
                        demuxer.removeAllListeners();
                    }

                    const audioTrack = data.audioTrack != null ? parseInt(data.audioTrack) : -1;
                    if (options.verbose) console.log(`[WS] Play: ${file.name} (audio track: ${audioTrack === -1 ? 'auto' : '#' + audioTrack})`);

                    demuxer = new AudioDemuxer(file.path, {
                        sampleRate: config.AUDIO_SAMPLE_RATE,
                        audioStreamIndex: audioTrack
                    });

                    demuxer.on('info', (info) => {
                        if (options.verbose) {
                            console.log(`[WS] Audio: stream #${info.trackIndex} ${info.codec} ${info.channels}ch ${info.layout} "${info.trackTitle}"`);
                        }
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(JSON.stringify({
                                type: 'info',
                                channels: info.channels,
                                sampleRate: info.sampleRate,
                                layout: info.layout,
                                duration: info.duration,
                                codec: info.codec,
                                trackTitle: info.trackTitle
                            }));
                        }
                    });

                    demuxer.on('data', ({ pts, buffer }) => {
                        if (ws.readyState !== WebSocket.OPEN) return;

                        /*
                         * Build binary frame:
                         * [8 bytes Float64BE PTS] [PCM data as-is (interleaved float32le)]
                         *
                         * The PCM is already channels * BLOCK_SIZE * 4 bytes of interleaved
                         * float32 LE from FFmpeg. The client will de-interleave as needed.
                         */
                        const header = Buffer.alloc(8);
                        header.writeDoubleBE(pts, 0);
                        const frame = Buffer.concat([header, buffer]);

                        try {
                            ws.send(frame, { binary: true });
                        } catch (e) {
                            /* Client disconnected */
                        }
                    });

                    demuxer.on('end', () => {
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(JSON.stringify({ type: 'end' }));
                        }
                    });

                    demuxer.on('error', (err) => {
                        if (options.verbose) console.error('[WS] Demuxer error:', err.message);
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(JSON.stringify({
                                type: 'error',
                                message: err.message
                            }));
                        }
                    });

                    try {
                        await demuxer.start();
                    } catch (err) {
                        if (options.verbose) console.error('[WS] Start error:', err.message);
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(JSON.stringify({
                                type: 'error',
                                message: err.message
                            }));
                        }
                    }
                    break;
                }

                case 'seek': {
                    if (!demuxer) return;
                    const time = parseFloat(data.time) || 0;
                    if (options.verbose) console.log(`[WS] Seek to ${time.toFixed(2)}s`);

                    try {
                        await demuxer.seek(time);
                        if (ws.readyState === WebSocket.OPEN) {
                            ws.send(JSON.stringify({ type: 'seek_done', pts: time }));
                        }
                    } catch (err) {
                        if (options.verbose) console.error('[WS] Seek error:', err.message);
                    }
                    break;
                }

                case 'stop': {
                    if (demuxer) {
                        demuxer.stop();
                        demuxer.removeAllListeners();
                        demuxer = null;
                    }
                    if (options.verbose) console.log('[WS] Stopped');
                    break;
                }
            }
        });

        ws.on('close', () => {
            if (options.verbose) console.log('[WS] Client disconnected');
            if (demuxer) {
                demuxer.stop();
                demuxer.removeAllListeners();
                demuxer = null;
            }
        });

        ws.on('error', (err) => {
            if (options.verbose) console.error('[WS] Error:', err.message);
        });
    });

    return wss;
}

module.exports = { createWsServer };
