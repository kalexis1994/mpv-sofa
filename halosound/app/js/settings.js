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
    constructor(audioEngine) {
        this.audioEngine = audioEngine;
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

        // Room preset
        const roomSel = document.getElementById('setting-room');
        if (roomSel) roomSel.addEventListener('change', (e) => {
            this.roomPreset = parseInt(e.target.value);
            this.audioEngine.setRoomPreset(this.roomPreset);
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

        this.applyOutputMode();
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
