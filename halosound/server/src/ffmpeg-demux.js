const { spawn, execFile } = require('child_process');
const EventEmitter = require('events');
const config = require('./config');

/**
 * AudioDemuxer - extracts multichannel PCM audio from video files using FFmpeg.
 *
 * Key features:
 * - Auto-selects best audio track (TrueHD Atmos > EAC3 7.1 > DTS-HD > AC3 5.1 > stereo)
 * - Real-time pacing: emits audio chunks at playback speed
 * - Backpressure: pauses FFmpeg stdout when send buffer is full
 * - Generation counter to avoid race conditions on seek
 */
class AudioDemuxer extends EventEmitter {
    constructor(filePath, options = {}) {
        super();
        this.filePath = filePath;
        this.sampleRate = options.sampleRate || config.AUDIO_SAMPLE_RATE;
        this.requestedStreamIndex = options.audioStreamIndex || -1; // -1 = auto-select
        /* Opt-in Atmos object extraction (see start() for why it's off by default) */
        this.extractObjects = options.extractObjects || process.env.HALO_EXTRACT_OBJECTS === '1';
        this.ffmpeg = null;
        this.channels = 0;
        this.duration = 0;
        this.layout = '';
        this.codec = '';
        this.audioStreamIndex = -1;
        this.sampleCount = 0;
        this.probed = false;
        this.residual = Buffer.alloc(0);

        this.sendQueue = [];
        this.pacingTimer = null;
        this.startTime = 0n;
        this.paused = false;
        this.stopped = false;
        this.generation = 0;      // incremented on each start, prevents stale handlers
        this.ffmpegDone = false;
        this.chunksEmitted = 0;

        // Declicker state: previous sample per channel for continuity across blocks
        this.prevSample = new Float32Array(16);
    }

    async probe() {
        return new Promise((resolve, reject) => {
            const args = [
                '-v', 'error',
                '-print_format', 'json',
                '-show_streams',
                '-show_format',
                this.filePath
            ];

            execFile(config.FFPROBE_PATH, args, { maxBuffer: 8 * 1024 * 1024 }, (err, stdout, stderr) => {
                if (err) {
                    const detail = (stderr || '').trim().slice(0, 400);
                    return reject(new Error(
                        `ffprobe failed (code ${err.code}): ${detail || err.message}`));
                }

                try {
                    const probe = JSON.parse(stdout);
                    const audioStreams = (probe.streams || []).filter(s => s.codec_type === 'audio');
                    if (audioStreams.length === 0) return reject(new Error('No audio stream found'));

                    const codecScore = {
                        'truehd': 100, 'dts': 70, 'eac3': 60, 'ac3': 50,
                        'flac': 45, 'aac': 40, 'vorbis': 35, 'opus': 35,
                        'mp3': 20, 'pcm_s16le': 30, 'pcm_s24le': 32, 'pcm_f32le': 34,
                    };

                    let bestStream = null;

                    /* Use requested stream index if provided */
                    if (this.requestedStreamIndex >= 0) {
                        bestStream = (probe.streams || []).find(s => s.index === this.requestedStreamIndex && s.codec_type === 'audio');
                    }

                    /* Otherwise auto-select best stream */
                    if (!bestStream) {
                        let bestScore = -1;
                        for (const s of audioStreams) {
                            const title = ((s.tags && s.tags.title) || '').toLowerCase();
                            if (title.includes('commentary')) continue;

                            let score = codecScore[s.codec_name] || 10;
                            score += (s.channels || 2) * 5;
                            if (s.codec_name === 'dts' && s.profile && s.profile.includes('MA')) score += 20;
                            const lang = (s.tags && s.tags.language) || '';
                            if (lang === 'eng' || lang === '') score += 10;

                            if (score > bestScore) {
                                bestScore = score;
                                bestStream = s;
                            }
                        }
                        if (!bestStream) bestStream = audioStreams[0];
                    }

                    this.audioStreamIndex = bestStream.index;
                    this.channels = bestStream.channels || 2;
                    this.layout = bestStream.channel_layout || `${this.channels}ch`;
                    this.codec = bestStream.codec_name;
                    this.duration = probe.format ? parseFloat(probe.format.duration) : 0;
                    this.probed = true;

                    resolve({
                        channels: this.channels,
                        sampleRate: this.sampleRate,
                        layout: this.layout,
                        duration: this.duration,
                        codec: this.codec,
                        trackIndex: this.audioStreamIndex,
                        trackTitle: (bestStream.tags && bestStream.tags.title) || ''
                    });
                } catch (e) {
                    reject(e);
                }
            });
        });
    }

    async start(seekSeconds = 0) {
        if (!this.probed) {
            await this.probe();
        }

        // Emit info after potential TrueHD channel override (done below in arg building)
        // We re-emit after setting up extract_objects so client gets correct channel count

        /* New generation - invalidates any stale close/data handlers */
        this.generation++;
        const gen = this.generation;

        this.stopped = false;
        this.ffmpegDone = false;
        this.chunksEmitted = 0;
        this.residual = Buffer.alloc(0);
        this.sendQueue = [];
        this.paused = false;

        /*
         * TrueHD Atmos handling. The ffmpeg extract_objects path (16ch: bed
         * + separated objects) produces audible micro-glitches on the object
         * channels (~600-1600 impulses/s, "rain"), so it is opt-in until the
         * clean Rust decoder is integrated. Default: normal 7.1 decode —
         * lossless and clean; objects are simply not separated (they are
         * already mixed into the 7.1 presentation by the format).
         */
        const isTrueHD = this.codec === 'truehd';
        const extract = isTrueHD && this.extractObjects;
        if (extract) {
            this.channels = 16;
            this.layout = '7.1.4+objects';
        } else if (isTrueHD) {
            this.channels = 8;
            this.layout = '7.1';
        }

        const blockBytes = config.AUDIO_BLOCK_SIZE * this.channels * 4;

        const args = [];
        if (seekSeconds > 0) {
            args.push('-ss', String(seekSeconds));
        }
        if (extract) {
            args.push('-extract_objects', '1');
        }
        args.push(
            '-i', this.filePath,
            '-map', `0:${this.audioStreamIndex}`,
            '-vn',
            '-acodec', 'pcm_f32le',
            '-f', 'f32le',
            '-ar', String(this.sampleRate),
            '-ac', String(this.channels),
            '-'
        );

        // Emit info with correct channel count (may have been updated for TrueHD)
        this.emit('info', {
            channels: this.channels,
            sampleRate: this.sampleRate,
            layout: this.layout,
            duration: this.duration,
            codec: this.codec,
            trackIndex: this.audioStreamIndex,
            trackTitle: ''
        });

        this.sampleCount = seekSeconds > 0 ? Math.floor(seekSeconds * this.sampleRate) : 0;

        this.ffmpeg = spawn(config.FFMPEG_PATH, args, {
            stdio: ['ignore', 'pipe', 'pipe']
        });

        this.ffmpeg.stderr.on('data', () => {});

        this.ffmpeg.stdout.on('data', (chunk) => {
            /* Stale generation check */
            if (gen !== this.generation || this.stopped) return;

            this.residual = Buffer.concat([this.residual, chunk]);

            while (this.residual.length >= blockBytes) {
                const block = Buffer.from(this.residual.subarray(0, blockBytes));
                this.residual = this.residual.subarray(blockBytes);

                // Declicker: only for the extract_objects path (its AU-boundary
                // glitches). On clean decodes it would mangle loud transients
                // (real deltas >0.10 happen constantly in action scenes).
                if (extract) {
                    this.declickBlock(block, config.AUDIO_BLOCK_SIZE, this.channels);
                }

                const pts = this.sampleCount / this.sampleRate;
                this.sampleCount += config.AUDIO_BLOCK_SIZE;

                this.sendQueue.push({ pts, buffer: block });
            }

            if (this.sendQueue.length > 800 && !this.paused && this.ffmpeg) {
                this.ffmpeg.stdout.pause();
                this.paused = true;
            }
        });

        this.ffmpeg.on('error', (err) => {
            if (gen !== this.generation) return;
            if (!this.stopped) this.emit('error', err);
        });

        this.ffmpeg.on('close', (code) => {
            /* CRITICAL: only act if this is still the current generation */
            if (gen !== this.generation) return;

            if (this.residual.length > 0 && !this.stopped) {
                const pts = this.sampleCount / this.sampleRate;
                const padded = Buffer.alloc(blockBytes, 0);
                this.residual.copy(padded);
                this.sendQueue.push({ pts, buffer: padded });
                this.residual = Buffer.alloc(0);
            }
            this.ffmpegDone = true;
        });

        this.startPacing(gen);
    }

    startPacing(gen) {
        this.stopPacing();
        this.startTime = process.hrtime.bigint();

        const chunkDurationMs = (config.AUDIO_BLOCK_SIZE / this.sampleRate) * 1000;
        // 2s lead: the client keeps a deep (~5s) jitter queue and its video
        // is slaved to the audio clock, so extra lead costs nothing in
        // perceived latency and rides through network/event-loop hiccups.
        const leadChunks = Math.ceil(2000 / chunkDurationMs);
        this.chunkDurationMs = chunkDurationMs;
        this.leadChunks = leadChunks;

        this.pacingTimer = setInterval(() => {
            if (gen !== this.generation || this.stopped) {
                this.stopPacing();
                return;
            }

            const elapsed = Number(process.hrtime.bigint() - this.startTime) / 1e6;
            let targetChunks = Math.floor(elapsed / chunkDurationMs) + leadChunks;

            /*
             * Never burst-catch-up after a stall (slow decoder, event-loop
             * hiccup, network backpressure).  Pacing against wall clock
             * means a stall would otherwise be followed by a flood of
             * chunks; the client's bounded queue then drops its oldest
             * entries and its audio clock jumps FORWARD, which the video
             * chases with a visible seek.  Instead, slide our clock so
             * delivery resumes at 1x with just the normal lead re-primed.
             */
            if (targetChunks > this.chunksEmitted + 2 * leadChunks) {
                this.startTime = process.hrtime.bigint() -
                    BigInt(Math.max(0, Math.round(this.chunksEmitted * chunkDurationMs * 1e6)));
                targetChunks = this.chunksEmitted + leadChunks;
            }

            while (this.chunksEmitted < targetChunks && this.sendQueue.length > 0) {
                const chunk = this.sendQueue.shift();
                this.emit('data', chunk);
                this.chunksEmitted++;
            }

            if (this.paused && this.sendQueue.length < 400 && this.ffmpeg) {
                this.ffmpeg.stdout.resume();
                this.paused = false;
            }

            if (this.ffmpegDone && this.sendQueue.length === 0) {
                this.stopPacing();
                if (!this.stopped && gen === this.generation) this.emit('end');
            }
        }, 4);
    }

    stopPacing() {
        if (this.pacingTimer) {
            clearInterval(this.pacingTimer);
            this.pacingTimer = null;
        }
    }

    /**
     * Declicker: detects and fixes discontinuities in interleaved PCM block.
     * The TrueHD extract_objects decoder produces multi-sample burst glitches
     * in BL(ch4)/BR(ch5) at AU boundaries. Two-pass approach:
     *   Pass 1: threshold=0.10 catches most glitch onsets
     *   Pass 2: threshold=0.15 mops up any remaining artifacts
     */
    declickBlock(buf, numSamples, numChannels) {
        const f32 = new Float32Array(buf.buffer, buf.byteOffset, numSamples * numChannels);

        // Two passes with decreasing thresholds for thorough cleanup
        const passes = [0.10, 0.15];

        for (const THRESHOLD of passes) {
            for (let ch = 0; ch < numChannels; ch++) {
                let prev = this.prevSample[ch];

                for (let i = 0; i < numSamples; i++) {
                    const idx = i * numChannels + ch;
                    const cur = f32[idx];
                    const jump = Math.abs(cur - prev);

                    if (jump > THRESHOLD) {
                        // Look ahead to find where signal stabilizes
                        let end = i + 1;
                        while (end < numSamples && end < i + 8) {
                            const nextIdx = end * numChannels + ch;
                            const prevIdx = (end - 1) * numChannels + ch;
                            const nextJump = Math.abs(f32[nextIdx] - f32[prevIdx]);
                            if (nextJump < THRESHOLD) break;
                            end++;
                        }

                        // Interpolate from prev to the stable sample
                        const endIdx = Math.min(end, numSamples - 1) * numChannels + ch;
                        const endVal = f32[endIdx];
                        const span = end - i + 1;
                        for (let j = i; j < end && j < numSamples; j++) {
                            const t = (j - i + 1) / span;
                            f32[j * numChannels + ch] = prev * (1 - t) + endVal * t;
                        }
                        i = end - 1;
                        prev = f32[(Math.min(end - 1, numSamples - 1)) * numChannels + ch];
                    } else {
                        prev = cur;
                    }
                }

                this.prevSample[ch] = prev;
            }
        }

        // Write back float32 to buffer
        Buffer.from(f32.buffer, f32.byteOffset, f32.byteLength).copy(buf);
    }

    /* Freeze emission while the client's video is buffering or paused */
    pause() {
        if (this.stopped || this.holdPaused) return;
        this.holdPaused = true;
        this.stopPacing();
    }

    resume() {
        if (!this.holdPaused || this.stopped) return;
        this.holdPaused = false;
        this.startPacing(this.generation);
        // Rewind the pacing clock so emission continues exactly where it
        // stopped (without re-injecting the lead buffer as extra latency).
        const emittedMs = (this.chunksEmitted - (this.leadChunks || 0)) * (this.chunkDurationMs || 5.33);
        this.startTime = process.hrtime.bigint() - BigInt(Math.max(0, Math.round(emittedMs * 1e6)));
    }

    async seek(seconds) {
        this.holdPaused = false;
        this.stop();
        await this.start(seconds);
    }

    stop() {
        this.stopped = true;
        this.stopPacing();
        if (this.ffmpeg) {
            try { this.ffmpeg.kill('SIGTERM'); } catch (e) {}
            this.ffmpeg = null;
        }
        this.residual = Buffer.alloc(0);
        this.sendQueue = [];
        this.paused = false;
    }
}

module.exports = { AudioDemuxer };
