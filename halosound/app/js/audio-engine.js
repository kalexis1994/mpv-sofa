// HaloSound Audio Engine - Web Audio API with WASM HRTF processing
class HaloAudioEngine {
    constructor() {
        this.ctx = null;
        this.workletNode = null;
        this.wasmReady = false;
        this.audioInfo = null;
        this.latency = 0;
        this.onLatencyMeasured = null;
    }

    async init() {
        this.ctx = new AudioContext({ sampleRate: 48000 });

        // Load AudioWorklet processor
        await this.ctx.audioWorklet.addModule('js/hrtf-worklet.js');

        // Create worklet node (stereo output)
        this.workletNode = new AudioWorkletNode(this.ctx, 'hrtf-processor', {
            outputChannelCount: [2],
            numberOfOutputs: 1
        });

        this.workletNode.connect(this.ctx.destination);

        // Handle messages from worklet
        this.workletNode.port.onmessage = (e) => {
            if (e.data.type === 'ready') {
                this.wasmReady = true;
                console.log('WASM HRTF engine ready in AudioWorklet');
            } else if (e.data.type === 'error') {
                console.warn('WASM worklet error:', e.data.message);
            } else if (e.data.type === 'latency') {
                this.latency = e.data.value;
                if (this.onLatencyMeasured) this.onLatencyMeasured(this.latency);
            }
        };

        // Measure latency
        this.latency = (this.ctx.baseLatency || 0) + (this.ctx.outputLatency || 0);
    }

    async loadWasm() {
        // Send WASM binary to worklet for initialization
        const wasmResp = await fetch('wasm/halosound_dsp.wasm');
        const wasmBytes = await wasmResp.arrayBuffer();
        this.workletNode.port.postMessage({ type: 'init-wasm', wasm: wasmBytes });
    }

    async loadSofa(url) {
        const resp = await fetch(url);
        const sofaData = await resp.arrayBuffer();
        this.workletNode.port.postMessage({ type: 'load-sofa', data: sofaData }, [sofaData]);
    }

    feedAudio(binaryData) {
        // binaryData: ArrayBuffer from WebSocket
        // Format: [8 bytes PTS float64] [N channels * 256 samples * 4 bytes float32]
        this.workletNode.port.postMessage({ type: 'audio', buffer: binaryData }, [binaryData]);
    }

    flush() {
        this.workletNode.port.postMessage({ type: 'flush' });
    }

    setLayout(layoutId) {
        this.workletNode.port.postMessage({ type: 'set-layout', layout: layoutId });
    }

    setRoomPreset(presetId) {
        this.workletNode.port.postMessage({ type: 'set-room-preset', preset: presetId });
    }

    setVolume(vol) {
        this.workletNode.port.postMessage({ type: 'set-volume', volume: vol });
    }

    setChannelMute(group, muted) {
        // group: 'bed' (ch 0-7) or 'height' (ch 8-11)
        this.workletNode.port.postMessage({ type: 'set-channel-mute', group, muted });
    }

    setOutputMode(mode) {
        // 'headphones' = HRTF active, 'speakers' = bypass (stereo downmix)
        this.workletNode.port.postMessage({ type: 'set-output-mode', mode });
    }

    setAudioInfo(info) {
        this.audioInfo = info;
        this.workletNode.port.postMessage({ type: 'audio-info', info });
    }

    getLatency() {
        return this.latency + 0.1; // Add 100ms jitter buffer
    }

    resume() {
        if (this.ctx && this.ctx.state === 'suspended') {
            return this.ctx.resume();
        }
    }

    destroy() {
        if (this.workletNode) {
            this.workletNode.disconnect();
            this.workletNode = null;
        }
        if (this.ctx) {
            this.ctx.close();
            this.ctx = null;
        }
    }
}
