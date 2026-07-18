// Connection manager - connects to HaloSound Server
class HaloConnection {
    constructor() {
        this.httpBase = null;  // 'http://ip:port'
        this.ws = null;        // WebSocket for audio
        this.wsBase = null;    // 'ws://ip:wsport'
        this.connected = false;
        this.bandwidthBps = 0; // measured LAN throughput (bits/s), 0 = unknown
        this.onConnected = null;
        this.onDisconnected = null;
        this.onAudioData = null;
        this.onAudioInfo = null;
        this.onError = null;
    }

    async connect(ip, httpPort = 8080, wsPort = 8081) {
        this.httpBase = `http://${ip}:${httpPort}`;
        this.wsBase = `ws://${ip}:${wsPort}`;

        // Test HTTP connection first
        try {
            const resp = await fetch(`${this.httpBase}/api/files`);
            if (!resp.ok) throw new Error('Server not responding');
            this.connected = true;
            if (this.onConnected) this.onConnected();
            return true;
        } catch (e) {
            if (this.onError) this.onError(e.message);
            return false;
        }
    }

    /*
     * Measure real download throughput from the server (~3.5s, XHR for wide
     * webOS support). The first 500ms are discarded (TCP ramp-up), so the
     * result reflects the steady state the video stream will actually get.
     * Stores bits/s in this.bandwidthBps; resolves with it (0 on failure).
     */
    measureBandwidth() {
        return new Promise((resolve) => {
            const xhr = new XMLHttpRequest();
            const t0 = performance.now();
            let startT = 0, startB = 0, lastT = 0, lastB = 0;
            const finish = () => {
                let bps = 0;
                if (startT && lastT - startT > 800) {
                    bps = ((lastB - startB) * 8) / ((lastT - startT) / 1000);
                }
                if (bps > 0) this.bandwidthBps = bps;
                resolve(bps);
            };
            xhr.open('GET', `${this.httpBase}/api/speedtest`);
            xhr.responseType = 'arraybuffer';
            xhr.onprogress = (e) => {
                const t = performance.now();
                if (!startT && t - t0 > 500) { startT = t; startB = e.loaded; }
                lastT = t; lastB = e.loaded;
                if (t - t0 > 3500) xhr.abort();
            };
            xhr.onabort = finish;
            xhr.onload = finish;
            xhr.onerror = () => resolve(0);
            xhr.send();
        });
    }

    async getFiles() {
        const resp = await fetch(`${this.httpBase}/api/files`);
        return resp.json();
    }

    async browse(dir) {
        const resp = await fetch(`${this.httpBase}/api/browse?dir=${encodeURIComponent(dir || '')}`);
        return resp.json();
    }

    async getFileInfo(fileId) {
        const resp = await fetch(`${this.httpBase}/api/files/${fileId}/info`);
        return resp.json();
    }

    getVideoUrl(fileId) {
        return `${this.httpBase}/video/${fileId}`;
    }

    connectAudio(fileId, audioTrack) {
        if (this.ws) this.ws.close();

        this.ws = new WebSocket(this.wsBase);
        this.ws.binaryType = 'arraybuffer';

        this.ws.onopen = () => {
            const msg = { type: 'play', fileId };
            if (audioTrack != null && audioTrack >= 0) {
                msg.audioTrack = audioTrack;
            }
            this.ws.send(JSON.stringify(msg));
        };

        this.ws.onmessage = (event) => {
            if (typeof event.data === 'string') {
                const msg = JSON.parse(event.data);
                if (msg.type === 'info' && this.onAudioInfo) {
                    this.onAudioInfo(msg);
                } else if (msg.type === 'seek_done' && this.onSeekDone) {
                    // The WS is ordered: every binary frame before this
                    // message is pre-seek audio, everything after is fresh.
                    this.onSeekDone(msg.pts);
                } else if (msg.type === 'error' && this.onAudioError) {
                    this.onAudioError(msg.message || 'Audio stream error');
                }
            } else {
                // Binary audio data
                if (this.onAudioData) this.onAudioData(event.data);
            }
        };

        this.ws.onerror = (e) => {
            if (this.onError) this.onError('WebSocket error');
        };

        this.ws.onclose = () => {
            console.log('Audio WebSocket closed');
        };
    }

    seekAudio(timeSeconds) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({ type: 'seek', time: timeSeconds }));
        }
    }

    pauseAudio() {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({ type: 'pause' }));
        }
    }

    resumeAudio() {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({ type: 'resume' }));
        }
    }

    stopAudio() {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify({ type: 'stop' }));
        }
        if (this.ws) {
            this.ws.close();
            this.ws = null;
        }
    }

    disconnect() {
        this.stopAudio();
        this.connected = false;
        if (this.onDisconnected) this.onDisconnected();
    }
}
