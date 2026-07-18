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

        this.bindVideoEvents();

        // Click-to-seek on progress bar
        const progressBar = document.getElementById('progress-bar');
        if (progressBar) {
            progressBar.addEventListener('click', (e) => {
                if (!this.playing && !this.currentFile) return;
                const rect = progressBar.getBoundingClientRect();
                const pct = (e.clientX - rect.left) / rect.width;
                const total = (this.engineMode === 'hls' && this.movieDuration)
                    ? this.movieDuration : (this.video.duration || 0);
                const time = pct * total;
                if (time >= 0) this.seek(time);
            });
        }
    }

    /* All persistent listeners for the video element, re-bound whenever the
     * element is recreated (see recreateVideoElement). */
    bindVideoEvents() {
        const alignTimer = (self) => {
            if (self.videoStarted && self.playing) self.alignAudioToVideo();
        };

        this.video.addEventListener('timeupdate', () => this.onTimeUpdate());
        this.video.addEventListener('ended', () => this.onEnded());

        // Hard media errors (broken stream, pipeline failure): fail cleanly
        // back to the library instead of letting webOS kill+reload the app
        // (which lands the user on the connect screen with no explanation).
        this.video.addEventListener('error', () => {
            if (!this.videoStarted && !this.currentFile) return;
            // Detached element (src removed) reports no error object —
            // those events are teardown noise, not failures.
            if (!this.video.error) return;
            const err = this.video.error;
            // The synthesized VOD playlist projects the segment tail, so the
            // last projected segment can 404 — an "error" within the final
            // seconds of the movie is just the end.
            if (this.engineMode === 'hls' && this.movieDuration > 0 &&
                this.absPosition() > this.movieDuration - 30) {
                this.onEnded();
                return;
            }
            const msg = `media error ${err.code}: ${(err.message || '').slice(0, 80)}`;
            console.error('[video]', msg);
            this.setDiag(msg);
            this.showLoading('Playback failed — returning to library');
            setTimeout(() => {
                this.hideLoading();
                this.stop();
                if (this.onPlaybackEnded) this.onPlaybackEnded();
            }, 2500);
        });

        // Video stalled: pause the audio pipeline too, so A/V can't drift
        // apart while the video buffers.  During a seek/starvation reacquire
        // the resume path owns the whole show — stay out of its way (its
        // release depends on the server refilling, which holdAudio would
        // pause, deadlocking the wait).
        this.video.addEventListener('waiting', () => {
            if (!this.videoStarted || this.audioStarved || this.pendingVideoSeek) return;
            this.videoStalls = (this.videoStalls || 0) + 1;
            this.showLoading('Buffering...');
            this.holdAudio(true);
        });
        this.video.addEventListener('playing', () => {
            // Phase A of a seek just revealed real playback: give the
            // pipeline a moment to settle on its true (keyframe-snapped)
            // position, then chase it with the audio (phase B).
            if (this.pendingVideoSeek) {
                this.pendingVideoSeek = false;
                setTimeout(() => alignTimer(this), 600);
                return;
            }
            this.hideLoading();
            if (this.playing) this.holdAudio(false);
        });
    }

    /*
     * webOS's media pipeline gets irreversibly stuck when an HLS source is
     * swapped on a reused <video> element (readyState freezes at 2, no
     * error, forever) — verified live: the same stalled session plays
     * instantly on a freshly-created element.  So every new HLS session
     * gets a brand-new element.
     */
    recreateVideoElement() {
        const old = this.video;
        const parent = old.parentElement;
        try { old.removeAttribute('src'); old.load(); } catch (e) {}
        const v = document.createElement('video');
        v.id = 'video-player';
        v.muted = true;
        v.setAttribute('preload', 'auto');
        parent.insertBefore(v, old);
        old.remove();
        this.video = v;
        this.bindVideoEvents();
    }

    async play(file, selection = {}) {
        this.engineMode = (() => {
            try { return localStorage.getItem('mpvsofa.engine') || 'hls'; }
            catch (e) { return 'hls'; }
        })();
        if (this.engineMode === 'hls') return this.playHls(file, selection);
        return this.playWs(file, selection);
    }

    /*
     * TV-NATIVE MODE: one HLS stream with server-side binaural rendering.
     * The TV's own player handles A/V sync (including its internal audio
     * pipeline and Bluetooth latency) — the whole two-clock problem class
     * from the low-latency mode simply doesn't exist here.
     */
    async playHls(file, selection = {}) {
        this.currentFile = file;
        this.selection = selection;
        this.movieDuration = selection.duration || 0;
        this.playing = true;
        this.videoStarted = false;
        this.sessionBase = 0;
        this.chapters = [];
        this.loadChapters();   // async, ticks appear when it lands
        this.showLoading('Preparing stream...');
        await this.startHlsSession(Math.max(0, selection.startAt || 0));
    }

    /* ---- Chapters: ticks on the timeline + current chapter label ------- */

    async loadChapters() {
        try {
            const r = await fetch(`${this.connection.httpBase}/api/files/${this.currentFile.id}/chapters`);
            this.chapters = (await r.json()) || [];
        } catch (e) { this.chapters = []; }
        this.renderChapterTicks();
    }

    renderChapterTicks() {
        const bar = document.getElementById('progress-bar');
        if (!bar) return;
        bar.querySelectorAll('.chapter-tick').forEach(el => el.remove());
        const dur = this.movieDuration || 0;
        if (!dur || !this.chapters || this.chapters.length < 2) return;
        for (const ch of this.chapters) {
            if (ch.start <= 0 || ch.start >= dur) continue;
            const tick = document.createElement('div');
            tick.className = 'chapter-tick';
            tick.style.left = (ch.start / dur * 100) + '%';
            bar.appendChild(tick);
        }
    }

    currentChapter(t) {
        if (!this.chapters || !this.chapters.length) return null;
        let cur = null;
        for (const ch of this.chapters) { if (ch.start <= t) cur = ch; else break; }
        return cur;
    }

    /* ---- Resume: remember position per file (mpv's watch-later) -------- */

    static RESUME_KEY = 'mpvsofa.resume';

    getResumeFor(name) {
        try {
            const all = JSON.parse(localStorage.getItem(HaloPlayer.RESUME_KEY) || '{}');
            return all[name] || null;
        } catch (e) { return null; }
    }

    saveResume() {
        if (!this.currentFile) return;
        const t = this.absPosition();
        const dur = this.movieDuration || this.video.duration || 0;
        try {
            const all = JSON.parse(localStorage.getItem(HaloPlayer.RESUME_KEY) || '{}');
            const name = this.currentFile.name;
            if (dur > 300 && t > 60 && t < dur * 0.95) {
                all[name] = { t: Math.floor(t), ts: Date.now() };
            } else if (dur > 0 && t >= dur * 0.95) {
                delete all[name];   // finished — no stale resume offer
            } else {
                return;             // too early / unknown duration: keep as-is
            }
            const names = Object.keys(all);
            if (names.length > 40) {
                names.sort((a, b) => (all[a].ts || 0) - (all[b].ts || 0));
                for (const n of names.slice(0, names.length - 40)) delete all[n];
            }
            localStorage.setItem(HaloPlayer.RESUME_KEY, JSON.stringify(all));
        } catch (e) {}
    }

    hlsParams(t) {
        const s = this.selection || {};
        const params = new URLSearchParams();
        params.set('audioTrack', s.audioTrack != null && s.audioTrack >= 0 ? s.audioTrack : 1);
        params.set('channels', Math.min(8, s.audioChannels || 6));   // clean decode (no object extraction)
        params.set('t', Math.max(0, t).toFixed(3));
        try {
            const sofa = localStorage.getItem('mpvsofa.hrtfProfile');
            if (sofa) params.set('sofa', sofa);
        } catch (e) {}
        try {
            if (localStorage.getItem('mpvsofa.audioMode') === 'original') {
                params.set('audioMode', 'original');
            }
        } catch (e) {}
        const roomSel = document.getElementById('setting-room');
        if (roomSel) params.set('room', roomSel.value);
        // Measured link throughput → server picks copy-vs-reencode + bitrate.
        if (this.connection.bandwidthBps > 0) {
            params.set('bw', Math.round(this.connection.bandwidthBps));
        }
        return params;
    }

    async startHlsSession(t) {
        // Fresh element per session: detaches the dying playlist (whose
        // session the server kills the moment we request a new one) AND
        // dodges the webOS pipeline-reuse freeze.
        this.recreateVideoElement();
        try {
            // Hard timeout: a dead/stale server must produce a clear error,
            // not an endless "Buffering..." hang.
            const ctrl = new AbortController();
            const timer = setTimeout(() => ctrl.abort(), 40000);
            let resp;
            try {
                resp = await fetch(
                    `${this.connection.httpBase}/api/files/${this.currentFile.id}/hls?${this.hlsParams(t)}`,
                    { signal: ctrl.signal });
            } finally { clearTimeout(timer); }
            if (!resp.ok) {
                let detail = 'HTTP ' + resp.status;
                try { detail = (await resp.json()).error || detail; } catch (e) {}
                throw new Error(detail);
            }
            const j = await resp.json();
            this.sessionBase = j.base || 0;
            this.attachHls(this.connection.httpBase + j.url);
        } catch (e) {
            console.error('HLS session failed:', e);
            const msg = e.name === 'AbortError' ? 'server not responding' : e.message;
            this.setDiag('HLS session failed: ' + msg);
            this.showLoading('Stream failed: ' + msg);
            setTimeout(() => this.hideLoading(), 5000);
            // Server unreachable → surface it as a designed reconnect flow
            // instead of a mystery hang.
            if (e.name === 'AbortError' || /Failed to fetch|NetworkError/i.test(e.message)) {
                if (this.onServerLost) this.onServerLost();
            }
        }
    }

    /*
     * Attach a session playlist to a fresh element and start playback.
     * Self-heals the webOS pipeline wedge: occasionally even a fresh
     * pipeline freezes (readyState pinned below 3 with seconds of data
     * buffered, currentTime stuck, no error). The same session plays fine
     * on another fresh element, so detect the signature and retry.
     */
    attachHls(url, attempt = 0) {
        this.recreateVideoElement();
        this.video.muted = false;              // audio rides IN the stream
        this.video.src = url;
        // The HLS player may jump to the "live edge" of the growing
        // EVENT playlist instead of starting at 0.  Snap back ONLY once
        // playback is actually rolling (playing + readyState>=3): seeking
        // the element any earlier re-triggers the webOS pipeline freeze.
        const v = this.video;
        const snap = () => {
            if (this.video === v && v.currentTime > 4 && v.readyState >= 3) {
                v.currentTime = 0;
            }
        };
        v.addEventListener('playing', () => { snap(); setTimeout(snap, 1500); }, { once: true });
        v.play().catch(() => {});
        this.videoStarted = true;
        this.attachSubsForSession();

        clearInterval(this.wedgeTimer);
        let stuck = 0, lastVt = -1;
        this.wedgeTimer = setInterval(() => {
            if (this.video !== v || !this.playing) { clearInterval(this.wedgeTimer); return; }
            const vt = v.currentTime;
            if (vt > 0 && vt !== lastVt) { clearInterval(this.wedgeTimer); return; }  // rolling
            lastVt = vt;
            const buf = v.buffered.length ? v.buffered.end(v.buffered.length - 1) - v.buffered.start(0) : 0;
            if (v.readyState < 3 && buf > 4) stuck++; else stuck = 0;
            if (stuck >= 3) {                  // ~6s frozen with data available
                clearInterval(this.wedgeTimer);
                if (attempt < 2) {
                    console.warn('[player] webOS pipeline wedge — retrying on a fresh element (' + (attempt + 1) + ')');
                    this.setDiag('pipeline wedge, retry ' + (attempt + 1));
                    this.attachHls(url, attempt + 1);
                } else {
                    this.setDiag('pipeline wedge: retries exhausted');
                    this.showLoading('TV media pipeline is stuck — if this repeats, restart the TV');
                }
            }
        }, 2000);
    }

    /* (Re)attach the selected subtitles shifted to the session timeline. */
    attachSubsForSession() {
        const sel = this.selection || {};
        this.clearSubtitles();
        if (sel.subtitlePgs != null) {
            this.attachPgs(this.currentFile.id, sel.subtitlePgs);
            if (this.pgsRenderer) this.pgsRenderer.timeOffset = this.sessionBase;
        } else if (sel.subtitleExt) {
            const url = sel.subtitleExt +
                (sel.subtitleExt.includes('?') ? '&' : '?') + 'offset=' + this.sessionBase.toFixed(3);
            this.addSubtitleUrl(url);
        } else if (sel.subtitleTrack >= 0) {
            this.addSubtitleUrl(
                `${this.connection.httpBase}/api/files/${this.currentFile.id}` +
                `/subtitles/${sel.subtitleTrack}?offset=${this.sessionBase.toFixed(3)}`);
        }
    }

    /* Restart the session at the current position (profile/room changed). */
    restartHls() {
        if (this.engineMode !== 'hls' || !this.videoStarted) return;
        const abs = this.sessionBase + this.video.currentTime;
        this.showLoading('Applying audio settings...');
        this.startHlsSession(abs);
    }

    async playWs(file, selection = {}) {
        this.currentFile = file;
        this.audioStartPts = -1;
        this.videoStarted = false;
        this.videoReady = false;
        this.audioReady = false;
        this.audioHold = false;
        this.audioStarved = false;
        this.starvedStats = 0;
        this.awaitingSeek = false;
        this.pendingSeeks = 0;
        this.pendingVideoSeek = false;
        this.reacquireServerPaused = false;
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

        // Subtitles: embedded text stream, external file, or image-based
        // PGS rendered onto a canvas overlay by libpgs.
        this.clearSubtitles();
        if (selection.subtitlePgs != null) {
            this.attachPgs(file.id, selection.subtitlePgs);
        } else if (selection.subtitleExt) {
            this.addSubtitleUrl(selection.subtitleExt);
        } else if (selection.subtitleTrack >= 0) {
            this.addSubtitleTrack(file.id, selection.subtitleTrack);
        }

        // Start audio connection with selected track
        this.connection.onAudioInfo = (audioInfo) => {
            this.audioEngine.setAudioInfo(audioInfo);
            if (this.onAudioInfo) this.onAudioInfo(audioInfo);
        };

        // Drop stale in-flight audio at the exact protocol boundary: chunks
        // sent before the server processed our seek keep arriving after our
        // local flush and would poison the reacquire anchor (the realign
        // then pins the video to PRE-seek audio and everything cascades).
        this.connection.onSeekDone = () => {
            this.audioEngine.flush();
            // Rapid consecutive seeks each arm the gate; only the LAST
            // seek_done opens it, so the release can't anchor to audio of
            // an intermediate seek target.
            this.pendingSeeks = Math.max(0, (this.pendingSeeks || 0) - 1);
            if (this.pendingSeeks === 0) this.awaitingSeek = false;
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
            // While reacquiring (audioStarved) this block OWNS the pipeline:
            // a stray audioHold from a video-stall event must not block the
            // release (that deadlocked rapid-seek sequences).
            if (this.videoStarted && this.playing && !this.pendingVideoSeek &&
                (this.audioStarved || !this.audioHold)) {
                if (s.queued === 0) {
                    this.starvedStats = (this.starvedStats || 0) + 1;
                    if (this.starvedStats >= 3 && !this.audioStarved) {
                        this.audioStarved = true;
                        this.audioStalls = (this.audioStalls || 0) + 1;
                        this.resumeAtChunks = 375;   // real starvation: deep refill (~2s)
                        this.showLoading('Buffering audio...');
                        this.video.pause();
                        // Freeze the worklet too (but NOT the server — it
                        // must keep sending to refill) so trickling data
                        // accumulates silently instead of stuttering out.
                        this.audioEngine.setHold(true);
                    }
                } else {
                    this.starvedStats = 0;

                    // Long reacquire (slow video buffering): stop the server
                    // from streaming the whole movie into our capped queue —
                    // overflow drops would advance the realign anchor and
                    // chase the video into never-buffered territory.
                    if (this.audioStarved && s.queued >= 750 && !this.reacquireServerPaused) {
                        this.reacquireServerPaused = true;
                        this.connection.pauseAudio();
                    }

                    // Release only when BOTH sides are ready: enough fresh
                    // audio AND a decodable video. Releasing on audio alone
                    // let the audio free-run ~5s whenever the post-seek video
                    // was still buffering ('waiting' had already fired during
                    // the reacquire and never re-fires, so nothing held it).
                    // Buffered runway: readyState alone lets a big-jump video
                    // start with barely any data — it then plays in judders
                    // (with no 'waiting' events) while audio runs free.
                    let runway = 0;
                    try {
                        const b = this.video.buffered, t = this.video.currentTime;
                        for (let i = 0; i < b.length; i++) {
                            if (b.start(i) <= t + 0.1 && b.end(i) > t) { runway = b.end(i) - t; break; }
                        }
                    } catch (e) { runway = 99; }

                    if (this.audioStarved && !this.awaitingSeek &&
                        s.queued >= (this.resumeAtChunks || 375) &&
                        this.video.readyState >= 3 && runway >= 3) {
                        this.audioStarved = false;
                        this.resumeAtChunks = 375;
                        if (this.reacquireServerPaused) {
                            this.reacquireServerPaused = false;
                            this.connection.resumeAudio();
                        }
                        this.hideLoading();
                        // NO video realign here: the video's position is the
                        // ground truth (webOS snaps video seeks to keyframes,
                        // so moving it would land somewhere else entirely).
                        // The queued audio was requested at exactly the
                        // video's position — release and play together.
                        this.holdAudio(false);       // release any stray video-stall hold
                        this.audioEngine.setHold(false);
                        // Short settle + a 12s convergence phase in which
                        // checkSync corrects faster (the video clock creeps
                        // while the decoder ramps up after a jump).
                        this.suppressSync(1500);
                        this.convergeUntil = performance.now() + 12000;
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
        if (!this.playing || !this.videoStarted || this.audioHold ||
            this.audioStarved || this.pendingVideoSeek) return;
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

        // Two phases:
        //  - convergence (12s after a seek release): the TV's video clock
        //    creeps while the decoder ramps — correct fast and often.
        //  - steady state: the video frame clock saw-tooths ±0.2s and the
        //    extrapolated audio clock adds noise, so require two CONSECUTIVE
        //    out-of-range checks before correcting (a spurious correction
        //    seek triggers rebuffering and cascades).
        const converging = now < (this.convergeUntil || 0);
        const threshold  = converging ? 0.35 : 0.5;
        const strikesReq = converging ? 1 : 2;
        const rateMs     = converging ? 2500 : 5000;

        if (Math.abs(residual) > threshold) {
            this.driftStrikes = (this.driftStrikes || 0) + 1;
        } else {
            this.driftStrikes = 0;
        }

        if (this.driftStrikes >= strikesReq && now - (this.lastHardSync || 0) > rateMs) {
            this.lastHardSync = now;
            this.driftStrikes = 0;
            console.log(`[sync] residual ${residual.toFixed(2)}s → re-aligning audio to video`);
            // NEVER seek the video to correct drift — webOS snaps it to the
            // previous keyframe (up to ~10s back).  Re-seek the audio.
            this.alignAudioToVideo();
            return;
        }

        // Fine trim: small persistent offsets (e.g. the video's play-ramp
        // after a release leaves a constant ~0.25s bias that sits below the
        // hard threshold forever).  Nudge the AUDIO queue — drop or pad a
        // few hundred ms, faded, no seeks, no rebuffering.
        if (Math.abs(residual) > 0.12) {
            this.trimStrikes = (this.trimStrikes || 0) + 1;
        } else {
            this.trimStrikes = 0;
        }
        if (this.trimStrikes >= 3 && now - (this.lastTrim || 0) > 4000) {
            this.lastTrim = now;
            this.trimStrikes = 0;
            console.log(`[sync] trim ${residual.toFixed(2)}s (audio nudge)`);
            this.audioEngine.nudge(residual);
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

    /*
     * Seek architecture: THE VIDEO IS THE POSITION MASTER.
     *
     * The webOS <video> pipeline snaps every seek to the previous keyframe
     * (up to ~10s back on long-GOP x265 encodes) and only reveals the TRUE
     * landing position a moment after playback resumes — seeking the video
     * to a computed position is therefore impossible.  The audio stream,
     * being sample-accurate, is the one that chases: after the video lands
     * we re-seek the SERVER to the video's actual position (phase B).
     */
    seek(seconds) {
        if (!this.videoStarted) return;
        if (this.engineMode === 'hls') return this.seekHls(seconds);
        this.suppressSync();
        this.showLoading('Buffering...');
        // Phase A: reposition the video and let it start playing (silently,
        // worklet held) so its keyframe-snapped true position materializes.
        this.pendingVideoSeek = true;
        this.audioEngine.flush();
        this.audioEngine.setHold(true);
        this.audioStarved = false;
        this.video.currentTime = seconds;
        this.video.play().catch(() => {});
    }

    /*
     * Phase B: align the sample-accurate audio to wherever the video
     * actually is.  Reuses the reacquire machinery: pause video, seek the
     * server to the matching pts, release when fresh audio + video runway
     * are both ready.  Also used for drift corrections — the video must
     * NEVER be corrected by seeking it (keyframe snap-back).
     */
    alignAudioToVideo() {
        if (!this.videoStarted) return;
        const base = this.syncBaseline !== null ? this.syncBaseline : 0;
        const target = Math.max(0, this.video.currentTime - base + this.userAudioDelay);
        this.audioStarved = true;
        this.starvedStats = 0;
        this.resumeAtChunks = 94;         // ~500ms cushion: stays snappy
        this.awaitingSeek = true;
        this.pendingSeeks = (this.pendingSeeks || 0) + 1;
        this.audioEngine.flush();
        this.audioEngine.setHold(true);
        this.showLoading('Buffering...');
        this.video.pause();
        this.connection.seekAudio(target);
    }

    /* Seek within the HLS session natively when the target region is
     * already transcoded; otherwise spin up a fresh session there. */
    seekHls(absSeconds) {
        const rel = absSeconds - this.sessionBase;
        let within = false;
        try {
            const sk = this.video.seekable;
            for (let i = 0; i < sk.length; i++) {
                if (rel >= sk.start(i) && rel <= sk.end(i) - 0.5) { within = true; break; }
            }
        } catch (e) {}
        if (rel >= 0 && within) {
            this.video.currentTime = rel;          // native, TV keeps sync
            return;
        }
        this.showLoading('Buffering...');
        this.startHlsSession(Math.max(0, absSeconds));
    }

    /* Current position on the MOVIE timeline (abs), regardless of mode. */
    absPosition() {
        return (this.engineMode === 'hls' ? this.sessionBase : 0) + this.video.currentTime;
    }

    seekRelative(delta) {
        this.seek(Math.max(0, this.absPosition() + delta));
    }

    stop() {
        clearTimeout(this.startTimeout);
        clearInterval(this.wedgeTimer);
        this.saveResume();
        this.hideLoading();
        if (this.engineMode === 'hls') {
            // Tear down the server-side transcode session.
            try { fetch(`${this.connection.httpBase}/api/hls/stop`); } catch (e) {}
            this.video.muted = true;   // ws mode expects a muted element
            this.sessionBase = 0;
        }
        this.audioHold = false;
        this.audioStarved = false;
        this.starvedStats = 0;
        this.resumeAtChunks = 375;
        this.awaitingSeek = false;
        this.pendingSeeks = 0;
        this.pendingVideoSeek = false;
        this.reacquireServerPaused = false;
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

    /*
     * Attach image-based (PGS) subtitles: the server extracts the raw .sup
     * stream and libpgs renders it onto a canvas overlaid on the video.
     * First selection of a track on a big file takes a while server-side
     * (the whole container must be demuxed once); it's cached after that.
     */
    attachPgs(fileId, streamIndex) {
        this.disposePgs();
        if (typeof libpgs === 'undefined') {
            this.setDiag('PGS renderer unavailable (libpgs missing)');
            return;
        }
        try {
            this.pgsRenderer = new libpgs.PgsRenderer({
                workerUrl: 'js/vendor/libpgs.worker.js',
                video: this.video,
                subUrl: `${this.connection.httpBase}/api/files/${fileId}/pgssub/${streamIndex}`,
            });
        } catch (e) {
            console.warn('PGS renderer failed:', e);
            this.setDiag('PGS subtitles failed: ' + e);
        }
    }

    disposePgs() {
        if (this.pgsRenderer) {
            try { this.pgsRenderer.dispose(); } catch (e) {}
            this.pgsRenderer = null;
        }
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
        this.disposePgs();
    }

    onTimeUpdate() {
        // In HLS mode the element's timeline is session-relative and its
        // duration only reflects what's been transcoded — display the
        // MOVIE timeline instead.
        const current = this.absPosition();
        const total = (this.engineMode === 'hls' && this.movieDuration)
            ? this.movieDuration : (this.video.duration || 0);
        document.getElementById('time-current').textContent = this.formatTime(current);
        document.getElementById('time-total').textContent = this.formatTime(total);
        if (total > 0) {
            const pct = (current / total) * 100;
            document.getElementById('progress-fill').style.width = pct + '%';
        }

        const chEl = document.getElementById('chapter-label');
        if (chEl) {
            const ch = this.currentChapter(current);
            chEl.textContent = ch ? (ch.title || '') : '';
        }

        // Persist position every ~5s so "Resume" works after closing.
        const now = Date.now();
        if (!this._lastResumeSave || now - this._lastResumeSave > 5000) {
            this._lastResumeSave = now;
            this.saveResume();
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
