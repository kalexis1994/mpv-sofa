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
            } else if (e.data.type === 'stats') {
                this.lastStats = e.data;
                if (this.onStats) this.onStats(e.data);
            }
        };

        // Measure latency
        this.latency = (this.ctx.baseLatency || 0) + (this.ctx.outputLatency || 0);
    }

    async loadWasm() {
        // Send WASM binary to worklet for initialization.
        // Emscripten minifies wasm import/export names differently on every
        // release, so we parse the generated JS glue for the real-name mapping
        // instead of hardcoding letters in the worklet.
        // Prefer the SIMD build (≈2-4× faster convolution); fall back to
        // the scalar build on engines without WASM SIMD support.
        const variants = ['halosound_dsp_simd', 'halosound_dsp'];
        let wasmBytes = null;
        let glue = '';
        for (const name of variants) {
            try {
                const resp = await fetch(`wasm/${name}.wasm`);
                if (!resp.ok) continue;
                const bytes = await resp.arrayBuffer();
                if (!WebAssembly.validate(bytes)) {
                    console.warn(`${name}.wasm not supported by this engine`);
                    continue;
                }
                wasmBytes = bytes;
                glue = await (await fetch(`wasm/${name}.js`)).text();
                console.log('WASM variant:', name);
                break;
            } catch (e) { /* try next variant */ }
        }
        if (!wasmBytes) throw new Error('No usable WASM variant found');

        const exportMap = {};   // realName -> minified export name
        const importMap = {};   // minified import name -> real name
        try {
            // _halo_create=Module["_halo_create"]=wasmExports["e"]
            for (const m of glue.matchAll(/Module\["_(\w+)"\]=wasmExports\["([\w$]+)"\]/g)) {
                exportMap[m[1]] = m[2];
            }
            // memory=wasmMemory=wasmExports["c"]
            const mem = glue.match(/wasmMemory=wasmExports\["([\w$]+)"\]/);
            if (mem) exportMap.memory = mem[1];
            // initRuntime(){...wasmExports["d"]()}  -> __wasm_call_ctors
            const ctors = glue.match(/initRuntime\(\)\{[^}]*wasmExports\["([\w$]+)"\]\(\)/);
            if (ctors) exportMap.wasm_call_ctors = ctors[1];
            // var wasmImports={a:___assert_fail,b:_emscripten_resize_heap}
            const imp = glue.match(/wasmImports=\{([^}]*)\}/);
            if (imp) {
                for (const pair of imp[1].split(',')) {
                    const [min, real] = pair.split(':');
                    if (min && real) importMap[min.trim()] = real.trim().replace(/^_+/, '');
                }
            }
        } catch (e) {
            console.warn('WASM glue parse failed; worklet will try unminified names', e);
        }

        this.workletNode.port.postMessage({
            type: 'init-wasm', wasm: wasmBytes, exportMap, importMap
        });
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

    /* Hold = output silence without consuming the queue (video buffering/pause) */
    setHold(on) {
        this.workletNode.port.postMessage({ type: 'set-hold', hold: !!on });
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
