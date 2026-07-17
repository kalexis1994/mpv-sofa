// HaloSound Video Player with Audio Sync
class HaloPlayer {
    constructor(connection, audioEngine) {
        this.connection = connection;
        this.audioEngine = audioEngine;
        this.video = document.getElementById('video-player');
        this.playing = false;
        this.currentFile = null;
        this.syncInterval = null;

        // User-set audio delay compensation (seconds).  BT headphones add
        // transmission latency invisible to both the video and audio clocks
        // — the user tunes this until lips match what they hear.  Persisted.
        this.userAudioDelay = 0;
        try {
            const d = parseFloat(localStorage.getItem('mpvsofa.audioDelayMs'));
            if (!isNaN(d)) this.userAudioDelay = d / 1000;
        } catch (e) { /* localStorage unavailable */ }
        this.onPlaybackEnded = null;
        this.onAudioInfo = null;    // callback when audio track info arrives
        this.audioStartPts = -1;
        this.videoStarted = false;

        this.video.addEventListener('timeupdate', () => this.onTimeUpdate());
        this.video.addEventListener('ended', () => this.onEnded());

        // Video stalled: pause the audio pipeline too, so A/V can't drift
        // apart while the video buffers.
        this.video.addEventListener('waiting', () => {
            if (!this.videoStarted) return;
            this.videoStalls = (this.videoStalls || 0) + 1;
            this.showLoading('Buffering...');
            this.holdAudio(true);
        });
        this.video.addEventListener('playing', () => {
            this.hideLoading();
            if (this.playing) this.holdAudio(false);
        });

        // Click-to-seek on progress bar
        const progressBar = document.getElementById('progress-bar');
        if (progressBar) {
            progressBar.addEventListener('click', (e) => {
                if (!this.playing && !this.currentFile) return;
                const rect = progressBar.getBoundingClientRect();
                const pct = (e.clientX - rect.left) / rect.width;
                const time = pct * (this.video.duration || 0);
                if (time >= 0) this.seek(time);
            });
        }
    }

    async play(file, selection = {}) {
        this.currentFile = file;
        this.audioStartPts = -1;
        this.videoStarted = false;
        this.videoReady = false;
        this.audioReady = false;
        this.audioHold = false;
        this.audioStarved = false;
        this.starvedStats = 0;
        this.syncBaseline = null;    // recalibrated per playback session
        this.driftSamples = [];

        this.showLoading('Loading video...');

        // Set video source (muted - audio comes from WebSocket/WASM)
        this.video.src = this.connection.getVideoUrl(file.id);
        this.video.muted = true;

        // Start once BOTH the video can play and audio is flowing.
        this.video.addEventListener('canplay', () => {
            this.videoReady = true;
            this.maybeStart();
        }, { once: true });

        // Fallback: if audio never arrives, start video anyway so the
        // user sees the problem instead of a frozen screen.
        clearTimeout(this.startTimeout);
        this.startTimeout = setTimeout(() => {
            if (!this.videoStarted && this.videoReady) {
                this.showLoading('Audio stream failed — starting video only');
                this.setDiag('HRTF: No audio!');
                this.videoStarted = true;
                this.video.play().catch(() => {});
                setTimeout(() => this.hideLoading(), 3000);
            }
        }, 8000);

        // Surface server-side audio errors (diagnostics live in Settings)
        this.connection.onAudioError = (msg) => {
            console.error('Audio error:', msg);
            this.setDiag('Audio error: ' + msg);
            this.showLoading('Audio error — see Settings > Diagnostics');
            setTimeout(() => this.hideLoading(), 4000);
        };

        // Subtitles: embedded stream index or an external file's URL
        this.clearSubtitles();
        if (selection.subtitleExt) {
            this.addSubtitleUrl(selection.subtitleExt);
        } else if (selection.subtitleTrack >= 0) {
            this.addSubtitleTrack(file.id, selection.subtitleTrack);
        }

        // Start audio connection with selected track
        this.connection.onAudioInfo = (audioInfo) => {
            this.audioEngine.setAudioInfo(audioInfo);
            if (this.onAudioInfo) this.onAudioInfo(audioInfo);
        };

        this.connection.onAudioData = (data) => {
            if (!this.audioReady && data.byteLength >= 8) {
                this.audioStartPts = new DataView(data).getFloat64(0, false);
                this.audioReady = true;
                // Video not decodable yet: freeze the audio pipeline so the
                // audio clock doesn't run ahead while the video warms up.
                if (!this.videoReady) this.holdAudio(true);
            }

            this.audioEngine.feedAudio(data);

            if (!this.videoStarted) this.maybeStart();
        };

        this.connection.connectAudio(file.id, selection.audioTrack);

        // Resume audio context (requires user gesture)
        await this.audioEngine.resume();

        this.playing = true;

        // Track the audio playback clock reported by the worklet
        this.lastAudioPts = -1;
        this.lastStatsAt = 0;
        this.audioEngine.onStats = (s) => {
            if (s.pts >= 0) {
                this.lastAudioPts = s.pts;
                this.lastStatsAt = performance.now();
            }

            // Audio starvation: the worklet queue ran dry while playing.
            // If we let the video keep going, its clock runs ahead of the
            // frozen audio clock and checkSync() ends up yanking it
            // backward, then forward again when data returns in a burst.
            // Audio is the master clock — so make the VIDEO wait instead:
            // pause it, and once the queue has refilled realign it to the
            // audio position and resume.  ~3 stats messages ≈ 250ms dry
            // before we react, so seek flushes don't false-trigger.
            if (this.videoStarted && this.playing && !this.audioHold) {
                if (s.queued === 0) {
                    this.starvedStats = (this.starvedStats || 0) + 1;
                    if (this.starvedStats >= 3 && !this.audioStarved) {
                        this.audioStarved = true;
                        this.audioStalls = (this.audioStalls || 0) + 1;
                        this.showLoading('Buffering audio...');
                        this.video.pause();
                        // Freeze the worklet too (but NOT the server — it
                        // must keep sending to refill) so trickling data
                        // accumulates silently instead of stuttering out.
                        this.audioEngine.setHold(true);
                    }
                } else {
                    this.starvedStats = 0;
                    // Resume only once a solid cushion is back (~2s), so a
                    // trickling refill can't cause starve/resume ping-pong.
                    if (this.audioStarved && s.queued >= 375) {
                        this.audioStarved = false;
                        this.hideLoading();
                        // Realign while still paused using the calibrated
                        // pipeline offset — this lands exactly where the
                        // video stopped, so resume is seamless.
                        if (s.pts >= 0 && this.syncBaseline !== null) {
                            const target = s.pts + this.syncBaseline - this.userAudioDelay;
                            if (Math.abs(this.video.currentTime - target) > 0.15) {
                                this.video.currentTime = Math.max(0, target);
                            }
                        }
                        this.audioEngine.setHold(false);
                        this.suppressSync();
                        this.video.play().catch(() => {});
                    }
                }
            }
            if (s.underruns !== this.lastUnderruns || s.dropped !== this.lastDropped) {
                console.log(`[audio] underruns=${s.underruns} dropped=${s.dropped} queued=${s.queued} busy=${s.busyPct}%`);
                this.lastUnderruns = s.underruns;
                this.lastDropped = s.dropped;
            }
            // On-screen diagnostics (throttled to 1/s)
            const now = performance.now();
            if (!this._statsUiAt || now - this._statsUiAt > 1000) {
                this._statsUiAt = now;
                const bufMs = Math.round(s.queued * 256 / 48);
                this.setDiag(`HRTF: ${this.audioEngine.wasmReady ? 'Active' : 'Fallback'}` +
                    ` · buf ${(bufMs / 1000).toFixed(1)}s · dsp ${s.busyPct}%` +
                    ` · vstall ${this.videoStalls || 0} · astall ${this.audioStalls || 0}` +
                    ` · under ${s.underruns} · drop ${s.dropped}` +
                    ` · delay ${(this.userAudioDelay * 1000).toFixed(0)}ms`);
            }
        };

        // Start sync monitoring (audio is the master clock)
        this.syncInterval = setInterval(() => this.checkSync(), 500);
    }

    /* Start playback once video is decodable and audio is flowing */
    maybeStart() {
        if (this.videoStarted || !this.videoReady || !this.audioReady) return;
        this.videoStarted = true;
        clearTimeout(this.startTimeout);

        if (this.audioStartPts > 0.5) {
            this.video.currentTime = this.audioStartPts;
        }
        this.video.play().catch(e => console.warn('Video play failed:', e));
        this.holdAudio(false);   // release the audio clock in sync with video
        this.hideLoading();
    }

    showLoading(text) {
        const el = document.getElementById('player-loading');
        if (el) el.classList.remove('hidden');
        const txt = document.getElementById('player-loading-text');
        if (txt) txt.textContent = text || 'Loading...';
    }

    hideLoading() {
        const el = document.getElementById('player-loading');
        if (el) el.classList.add('hidden');
    }

    /**
     * A/V drift correction, TV-friendly.
     *
     * The audio clock (worklet PTS) runs a CONSTANT distance ahead of the
     * video clock: the TV's internal audio pipeline adds latency the Web
     * Audio API can't see (~0.4s on an LG B5 beyond the reported
     * baseLatency+outputLatency).  So instead of assuming offset 0, we
     * CALIBRATE it: median drift over the first ~6s of playback becomes
     * the baseline, and only deviations from that baseline are real
     * desync.  Corrections are hard seeks (no playbackRate — hardware
     * pipelines stutter on it), rate-limited to one per 5s.
     */
    checkSync() {
        if (!this.playing || !this.videoStarted || this.audioHold || this.audioStarved) return;
        if (this.lastAudioPts < 0 || this.video.paused || this.video.seeking) return;
        // Stale stats (e.g. right after a seek flush) → don't extrapolate.
        if (performance.now() - this.lastStatsAt > 1000) return;
        // Settling window after disruptive events (seek, buffering recovery,
        // starvation resume): both clocks are re-synchronizing — measuring
        // drift now yields garbage and mid-play corrections cause visible
        // jumps. Wait for steady state.
        if (performance.now() < (this.syncSuppressUntil || 0)) return;

        // Extrapolate the audio clock since the last stats message
        const audioPts = this.lastAudioPts + (performance.now() - this.lastStatsAt) / 1000;
        const drift = this.video.currentTime - audioPts;

        // Calibration phase: collect samples, adopt the median as baseline.
        if (this.syncBaseline === null) {
            this.driftSamples.push(drift);
            if (this.driftSamples.length >= 12) {
                const sorted = [...this.driftSamples].sort((a, b) => a - b);
                this.syncBaseline = sorted[Math.floor(sorted.length / 2)];
                console.log(`[sync] baseline calibrated: ${this.syncBaseline.toFixed(3)}s`);
            }
            return;
        }

        // userAudioDelay shifts the video back so it matches audio that is
        // HEARD late (BT headphone transmission latency).
        const residual = drift - this.syncBaseline + this.userAudioDelay;
        const now = performance.now();
        if (Math.abs(residual) > 0.4 && now - (this.lastHardSync || 0) > 5000) {
            this.lastHardSync = now;
            console.log(`[sync] residual ${residual.toFixed(2)}s → correcting video`);
            this.video.currentTime = audioPts + this.syncBaseline - this.userAudioDelay;
        }
    }

    /* Hold/release the whole audio path: worklet output + server pacing */
    holdAudio(on) {
        if (this.audioHold === on) return;
        this.audioHold = on;
        this.audioEngine.setHold(on);
        if (on) this.connection.pauseAudio();
        else {
            this.connection.resumeAudio();
            this.suppressSync();
        }
    }

    /* Pause drift corrections while the pipeline re-stabilizes. */
    suppressSync(ms = 4000) {
        this.syncSuppressUntil = performance.now() + ms;
    }

    /* Diagnostics line (lives in Settings > Diagnostics). */
    setDiag(text) {
        const el = document.getElementById('diag-line');
        if (el) el.textContent = text;
    }

    /*
     * Set the audio delay compensation in milliseconds and apply the change
     * to a live video immediately (nudge, no waiting for checkSync).
     */
    setAudioDelay(ms) {
        const newDelay = Math.max(0, ms) / 1000;
        const delta = newDelay - this.userAudioDelay;
        this.userAudioDelay = newDelay;
        try { localStorage.setItem('mpvsofa.audioDelayMs', String(Math.round(newDelay * 1000))); }
        catch (e) { /* localStorage unavailable */ }
        if (this.videoStarted && Math.abs(delta) > 0.001) {
            this.video.currentTime = Math.max(0, this.video.currentTime - delta);
            this.suppressSync(2000);
        }
    }

    pause() {
        if (this.playing) {
            this.video.pause();
            this.holdAudio(true);
            this.playing = false;
        }
    }

    resume() {
        if (!this.playing && this.currentFile) {
            this.video.play();
            this.holdAudio(false);
            this.playing = true;
        }
    }

    togglePlayPause() {
        if (this.playing) this.pause();
        else this.resume();
    }

    seek(seconds) {
        // Flush old audio from worklet queue before seeking
        this.audioEngine.flush();
        this.suppressSync();
        this.video.currentTime = seconds;
        this.connection.seekAudio(seconds);
    }

    seekRelative(delta) {
        this.seek(Math.max(0, this.video.currentTime + delta));
    }

    stop() {
        clearTimeout(this.startTimeout);
        this.hideLoading();
        this.audioHold = false;
        this.audioStarved = false;
        this.starvedStats = 0;
        this.video.pause();
        this.clearSubtitles();
        this.video.removeAttribute('src');
        this.video.load();
        this.connection.stopAudio();
        this.playing = false;
        this.currentFile = null;
        this.videoStarted = false;
        if (this.syncInterval) {
            clearInterval(this.syncInterval);
            this.syncInterval = null;
        }
    }

    addSubtitleTrack(fileId, streamIndex) {
        this.addSubtitleUrl(`${this.connection.httpBase}/api/files/${fileId}/subtitles/${streamIndex}`);
    }

    /* Attach a subtitle track from any VTT URL (embedded or external file). */
    addSubtitleUrl(url) {
        const track = document.createElement('track');
        track.kind = 'subtitles';
        track.label = 'Subtitles';
        track.srclang = 'en';
        track.src = url;
        track.default = true;
        this.video.appendChild(track);
        // Enable the track after adding
        this.video.addEventListener('loadedmetadata', () => {
            if (this.video.textTracks.length > 0) {
                this.video.textTracks[0].mode = 'showing';
            }
        }, { once: true });
    }

    clearSubtitles() {
        const tracks = this.video.querySelectorAll('track');
        tracks.forEach(t => t.remove());
    }

    onTimeUpdate() {
        const current = this.video.currentTime;
        const total = this.video.duration || 0;
        document.getElementById('time-current').textContent = this.formatTime(current);
        document.getElementById('time-total').textContent = this.formatTime(total);
        if (total > 0) {
            const pct = (current / total) * 100;
            document.getElementById('progress-fill').style.width = pct + '%';
        }
    }

    onEnded() {
        this.stop();
        if (this.onPlaybackEnded) this.onPlaybackEnded();
    }

    formatTime(s) {
        if (isNaN(s)) return '0:00';
        const h = Math.floor(s / 3600);
        const m = Math.floor((s % 3600) / 60);
        const sec = Math.floor(s % 60);
        if (h > 0) {
            return h + ':' + (m < 10 ? '0' : '') + m + ':' + (sec < 10 ? '0' : '') + sec;
        }
        return m + ':' + (sec < 10 ? '0' : '') + sec;
    }
}
