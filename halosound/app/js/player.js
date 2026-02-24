// HaloSound Video Player with Audio Sync
class HaloPlayer {
    constructor(connection, audioEngine) {
        this.connection = connection;
        this.audioEngine = audioEngine;
        this.video = document.getElementById('video-player');
        this.playing = false;
        this.currentFile = null;
        this.syncInterval = null;
        this.audioLatencyOffset = 0;
        this.onPlaybackEnded = null;
        this.onAudioInfo = null;    // callback when audio track info arrives
        this.audioStartPts = -1;
        this.videoStarted = false;

        this.video.addEventListener('timeupdate', () => this.onTimeUpdate());
        this.video.addEventListener('ended', () => this.onEnded());

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

        // Set video source (muted - audio comes from WebSocket/WASM)
        this.video.src = this.connection.getVideoUrl(file.id);
        this.video.muted = true;

        // Add subtitle track if selected
        this.clearSubtitles();
        if (selection.subtitleTrack >= 0) {
            this.addSubtitleTrack(file.id, selection.subtitleTrack);
        }

        // Start audio connection with selected track
        this.connection.onAudioInfo = (audioInfo) => {
            this.audioEngine.setAudioInfo(audioInfo);
            if (this.onAudioInfo) this.onAudioInfo(audioInfo);
            const infoStr = `${audioInfo.codec || ''} ${audioInfo.channels}ch ${audioInfo.layout} ${audioInfo.sampleRate}Hz`;
            document.getElementById('audio-info').textContent = infoStr;
            if (audioInfo.trackTitle) {
                document.getElementById('audio-info').textContent += ' | ' + audioInfo.trackTitle;
            }
        };

        this.connection.onAudioData = (data) => {
            let pts = 0;
            if (!this.videoStarted && data.byteLength >= 8) {
                pts = new DataView(data).getFloat64(0, false);
            }

            this.audioEngine.feedAudio(data);

            if (!this.videoStarted) {
                this.audioStartPts = pts;
                this.videoStarted = true;
                setTimeout(() => {
                    this.video.currentTime = this.audioStartPts;
                    this.video.play().catch(e => console.warn('Video play failed:', e));
                }, 150);
            }
        };

        this.connection.connectAudio(file.id, selection.audioTrack);

        // Resume audio context (requires user gesture)
        await this.audioEngine.resume();

        this.playing = true;

        // Update HRTF status
        document.getElementById('hrtf-status').textContent =
            'HRTF: ' + (this.audioEngine.wasmReady ? 'Active' : 'Fallback');

        // Start sync monitoring
        this.syncInterval = setInterval(() => this.checkSync(), 2000);
    }

    checkSync() {
        // Placeholder for A/V drift correction
        // In practice, the server pacing keeps audio roughly in sync
        // Fine-tuning would compare video.currentTime with audio PTS
    }

    pause() {
        if (this.playing) {
            this.video.pause();
            this.playing = false;
        }
    }

    resume() {
        if (!this.playing && this.currentFile) {
            this.video.play();
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
        this.video.currentTime = seconds;
        this.connection.seekAudio(seconds);
    }

    seekRelative(delta) {
        this.seek(Math.max(0, this.video.currentTime + delta));
    }

    stop() {
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
        const track = document.createElement('track');
        track.kind = 'subtitles';
        track.label = 'Subtitles';
        track.srclang = 'en';
        track.src = `${this.connection.httpBase}/api/files/${fileId}/subtitles/${streamIndex}`;
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
