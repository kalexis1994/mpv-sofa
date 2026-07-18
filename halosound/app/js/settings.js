// HaloSound Settings Manager
//
// Output modes:
//   "headphones" — HRTF binaural virtualization (default)
//   "speakers"   — Bypass HRTF, simple stereo downmix
//
// Speaker layout is auto-detected from the audio track:
//   2ch  → Stereo         (layout 0)
//   6ch  → 5.1            (layout 1)
//   8ch  → 7.1            (layout 2)
//  12ch  → 7.1.4          (layout 3)
//  16ch  → 7.1.4+Objects  (layout 4, Atmos with extract_objects)

class HaloSettings {
    constructor(audioEngine, player, connection) {
        this.audioEngine = audioEngine;
        this.player = player || null;
        this.connection = connection || null;
        this.outputMode = 'headphones';
        this.roomPreset = 1;
        this.reverbWet = 7;
        this.reverbDecay = 45;
        this.volume = 80;

        // Auto-detected from audio track
        this.detectedLayout = -1;
        this.detectedChannels = 0;
        this.detectedCodec = '';
        this.detectedTrackTitle = '';

        this.initControls();
    }

    initControls() {
        // Output mode toggle
        const modeGroup = document.getElementById('setting-output-mode');
        if (modeGroup) {
            modeGroup.querySelectorAll('.toggle-btn').forEach(btn => {
                btn.addEventListener('click', () => {
                    modeGroup.querySelectorAll('.toggle-btn').forEach(b => b.classList.remove('active'));
                    btn.classList.add('active');
                    this.outputMode = btn.dataset.value;
                    this.applyOutputMode();
                });
            });
        }

        // Playback engine (TV-native HLS vs low-latency WS+worklet)
        const engGroup = document.getElementById('setting-engine');
        if (engGroup) {
            let engine = 'hls';
            try { engine = localStorage.getItem('mpvsofa.engine') || 'hls'; } catch (e) {}
            engGroup.querySelectorAll('.toggle-btn').forEach(btn => {
                btn.classList.toggle('active', btn.dataset.value === engine);
                btn.addEventListener('click', () => {
                    engGroup.querySelectorAll('.toggle-btn').forEach(b => b.classList.remove('active'));
                    btn.classList.add('active');
                    try { localStorage.setItem('mpvsofa.engine', btn.dataset.value); } catch (e) {}
                    const hint = document.getElementById('hint-engine');
                    if (hint) hint.textContent = btn.dataset.value === 'hls'
                        ? 'TV-native: server renders binaural audio into the stream — applies on next playback'
                        : 'Low-latency: client-side rendering, instant profile switching — applies on next playback';
                });
            });
        }

        // Audio output: binaural render vs original passthrough (TV decodes,
        // Atmos lights up on E-AC-3/JOC tracks). TV-native mode only.
        const amGroup = document.getElementById('setting-audio-mode');
        if (amGroup) {
            let mode = 'binaural';
            try { mode = localStorage.getItem('mpvsofa.audioMode') || 'binaural'; } catch (e) {}
            const amHint = (m) => {
                const hint = document.getElementById('hint-audio-mode');
                if (hint) hint.textContent = m === 'original'
                    ? 'Original: untouched track, the TV decodes it (Atmos on Dolby Digital Plus tracks; TrueHD/DTS become DD+ 5.1)'
                    : 'Binaural: HRTF spatialization rendered by the server';
            };
            amHint(mode);
            amGroup.querySelectorAll('.toggle-btn').forEach(btn => {
                btn.classList.toggle('active', btn.dataset.value === mode);
                btn.addEventListener('click', () => {
                    amGroup.querySelectorAll('.toggle-btn').forEach(b => b.classList.remove('active'));
                    btn.classList.add('active');
                    try { localStorage.setItem('mpvsofa.audioMode', btn.dataset.value); } catch (e) {}
                    amHint(btn.dataset.value);
                    if (this.onEngineParamsChanged) this.onEngineParamsChanged();
                });
            });
        }

        // Room preset
        const roomSel = document.getElementById('setting-room');
        if (roomSel) roomSel.addEventListener('change', (e) => {
            this.roomPreset = parseInt(e.target.value);
            this.audioEngine.setRoomPreset(this.roomPreset);
            if (this.onEngineParamsChanged) this.onEngineParamsChanged();
        });

        // Reverb sliders
        const wetSlider = document.getElementById('setting-reverb-wet');
        if (wetSlider) wetSlider.addEventListener('input', (e) => {
            this.reverbWet = parseInt(e.target.value);
        });

        const decaySlider = document.getElementById('setting-reverb-decay');
        if (decaySlider) decaySlider.addEventListener('input', (e) => {
            this.reverbDecay = parseInt(e.target.value);
        });

        // Volume
        const volSlider = document.getElementById('setting-volume');
        if (volSlider) volSlider.addEventListener('input', (e) => {
            this.volume = parseInt(e.target.value);
            this.audioEngine.setVolume(this.volume / 100);
        });

        // Audio delay (BT headphone lip-sync compensation)
        const delaySlider = document.getElementById('setting-audio-delay');
        if (delaySlider) {
            if (this.player) delaySlider.value = Math.round(this.player.userAudioDelay * 1000);
            this._updateDelayHint();
            delaySlider.addEventListener('input', (e) => {
                if (this.player) this.player.setAudioDelay(parseInt(e.target.value, 10) || 0);
                this._updateDelayHint();
            });
        }

        // HRTF profile picker (populated from the server after connect)
        const hrtfSel = document.getElementById('setting-hrtf-profile');
        if (hrtfSel) {
            hrtfSel.addEventListener('change', () => this.applyHrtfProfile(hrtfSel.value));
        }

        this.applyOutputMode();
        this.detectBtOutput();
    }

    _updateDelayHint(suffix) {
        const hint = document.getElementById('hint-audio-delay');
        const slider = document.getElementById('setting-audio-delay');
        if (hint && slider) {
            hint.textContent = `${slider.value} ms${suffix ? ' · ' + suffix : ''}` +
                ' — increase until lips match what you hear';
        }
    }

    /**
     * Detect a Bluetooth sound output via the webOS Luna audio service.
     * If BT is active and the user never set a delay, seed a typical BT
     * (SBC codec) latency so lip-sync starts close.  Fails silently on
     * platforms without PalmServiceBridge or the service.
     */
    detectBtOutput() {
        if (typeof PalmServiceBridge === 'undefined') return;
        const uris = [
            'luna://com.webos.service.audio/getSoundOutput',
            'luna://com.webos.audio/getSoundOutput',
        ];
        const tryUri = (i) => {
            if (i >= uris.length) return;
            try {
                const bridge = new PalmServiceBridge();
                bridge.onservicecallback = (raw) => {
                    try {
                        const r = JSON.parse(raw);
                        if (r.returnValue === false || r.errorCode) { tryUri(i + 1); return; }
                        const out = String(r.soundOutput || r.soundOut || '').toLowerCase();
                        console.log('[audio-out]', out || raw.slice(0, 120));
                        if (out.includes('bt') || out.includes('bluetooth') || out.includes('headset')) {
                            let saved = null;
                            try { saved = localStorage.getItem('mpvsofa.audioDelayMs'); } catch (e) {}
                            if (saved === null && this.player) {
                                this.player.setAudioDelay(200);   // typical SBC latency
                                const slider = document.getElementById('setting-audio-delay');
                                if (slider) slider.value = 200;
                            }
                            this._updateDelayHint(`BT output detected (${out})`);
                        }
                    } catch (e) { tryUri(i + 1); }
                };
                bridge.call(uris[i], '{}');
            } catch (e) { /* bridge unavailable */ }
        };
        tryUri(0);
    }

    /**
     * Populate the HRTF profile picker from the server and load the saved
     * choice (or the built-in default).  Called once after connecting.
     */
    async initHrtf() {
        const sel = document.getElementById('setting-hrtf-profile');
        let saved = '';
        try { saved = localStorage.getItem('mpvsofa.hrtfProfile') || ''; } catch (e) {}

        if (sel && this.connection && this.connection.httpBase) {
            try {
                const resp = await fetch(`${this.connection.httpBase}/api/hrtf`);
                const profiles = await resp.json();
                // Rebuild options: built-in + server profiles
                sel.innerHTML = '<option value="">Built-in (MIT KEMAR)</option>';
                for (const p of profiles) {
                    const opt = document.createElement('option');
                    opt.value = p.name;
                    opt.textContent = p.name.replace(/\.sofa$/i, '');
                    sel.appendChild(opt);
                }
                if (saved && profiles.some(p => p.name === saved)) {
                    sel.value = saved;
                } else {
                    saved = '';
                }
            } catch (e) {
                console.warn('HRTF profile list unavailable:', e);
                saved = '';
            }
        }
        await this.applyHrtfProfile(saved, /*skipPersist=*/true);
    }

    async applyHrtfProfile(name, skipPersist) {
        const url = name
            ? `${this.connection.httpBase}/api/hrtf/${encodeURIComponent(name)}`
            : 'assets/hrtf/default.sofa';
        try {
            await this.audioEngine.loadSofa(url);
            console.log('HRTF profile loaded:', name || 'built-in');
            if (!skipPersist) {
                try { localStorage.setItem('mpvsofa.hrtfProfile', name || ''); } catch (e) {}
                if (this.onEngineParamsChanged) this.onEngineParamsChanged();
            }
        } catch (e) {
            console.warn('HRTF profile load failed, keeping current:', e);
        }
    }

    /**
     * Called when we receive audio info from the server.
     * Auto-detects layout and updates the UI.
     */
    updateAudioInfo(info) {
        this.detectedChannels = info.channels || 2;
        this.detectedCodec = info.codec || '';
        this.detectedTrackTitle = info.trackTitle || '';

        // Map channel count to layout ID
        if (this.detectedChannels >= 16) {
            this.detectedLayout = 4;  // 7.1.4 + Objects (Atmos extract_objects)
        } else if (this.detectedChannels >= 12) {
            this.detectedLayout = 3;  // 7.1.4 Atmos
        } else if (this.detectedChannels >= 8) {
            this.detectedLayout = 2;  // 7.1
        } else if (this.detectedChannels >= 6) {
            this.detectedLayout = 1;  // 5.1
        } else {
            this.detectedLayout = 0;  // Stereo
        }

        // Apply to engine
        this.audioEngine.setLayout(this.detectedLayout);

        // Update info display
        const layoutNames = ['Stereo', '5.1 Surround', '7.1 Surround', '7.1.4 Atmos', '7.1.4 + Objects'];
        const layoutName = layoutNames[this.detectedLayout] || `${this.detectedChannels}ch`;
        const codecName = this.formatCodec(this.detectedCodec);

        const infoEl = document.getElementById('setting-track-info');
        if (infoEl) {
            let text = `${codecName} ${layoutName}`;
            if (this.detectedTrackTitle) {
                text += `\n${this.detectedTrackTitle}`;
            }
            infoEl.textContent = text;
        }

        const hintEl = document.getElementById('hint-layout');
        if (hintEl) {
            hintEl.textContent = `${this.detectedChannels} channels detected → ${layoutName} layout applied`;
        }
    }

    formatCodec(codec) {
        const names = {
            'truehd': 'Dolby TrueHD',
            'eac3': 'Dolby Digital Plus',
            'ac3': 'Dolby Digital',
            'dts': 'DTS',
            'aac': 'AAC',
            'flac': 'FLAC',
            'pcm_s16le': 'PCM 16-bit',
            'pcm_s24le': 'PCM 24-bit',
            'pcm_f32le': 'PCM 32-bit float',
            'vorbis': 'Vorbis',
            'opus': 'Opus',
        };
        return names[codec] || codec.toUpperCase();
    }

    /**
     * Apply output mode: enable/disable HRTF-related settings.
     */
    applyOutputMode() {
        const isHeadphones = this.outputMode === 'headphones';

        // Enable/disable room and reverb groups
        const roomGroup = document.getElementById('group-room');
        const reverbGroup = document.getElementById('group-reverb');
        if (roomGroup) roomGroup.classList.toggle('disabled', !isHeadphones);
        if (reverbGroup) reverbGroup.classList.toggle('disabled', !isHeadphones);

        // Update hint
        const hintEl = document.getElementById('hint-output-mode');
        if (hintEl) {
            hintEl.textContent = isHeadphones
                ? 'HRTF binaural virtualization active'
                : 'Stereo downmix — no HRTF processing';
        }

        // Tell the audio engine
        this.audioEngine.setOutputMode(isHeadphones ? 'headphones' : 'speakers');
    }

    getState() {
        return {
            outputMode: this.outputMode,
            detectedLayout: this.detectedLayout,
            roomPreset: this.roomPreset,
            reverbWet: this.reverbWet,
            reverbDecay: this.reverbDecay,
            volume: this.volume
        };
    }
}
