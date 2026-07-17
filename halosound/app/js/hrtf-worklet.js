// HaloSound HRTF AudioWorklet Processor
//
// CRITICAL: halo_process() requires exactly 256 samples (HRTF_BLOCK_SIZE).
// Web Audio calls process() with 128-sample render quanta.
// We accumulate 256 input samples before calling WASM, then drain output
// over two process() calls.
//
// WASM export/import names are minified by Emscripten and change between
// releases. The main thread (audio-engine.js) parses the JS glue and sends
// {exportMap, importMap} along with the binary; we resolve real names first
// and fall back to the mapped minified names.

const HRTF_BLOCK = 256;   // Must match HRTF_BLOCK_SIZE in C code
const RENDER_QUANTUM = 128; // Web Audio standard

class HrtfProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.exports = null;
        this.fn = null;        // resolved export functions (real names)
        this.memory = null;
        this.enginePtr = 0;
        this.wasmReady = false;
        this.numChannels = 2;
        this.volume = 0.8;

        // --- Audio queue (from WebSocket) ---
        this.audioQueue = [];

        // --- Accumulation buffer: collects 256 interleaved input samples ---
        this.accumBuf = null;       // Float32Array(HRTF_BLOCK * numChannels)
        this.accumPos = 0;          // samples accumulated so far (0..255)

        // --- Output ring: holds 256 stereo samples from last halo_process ---
        this.outBufL = new Float32Array(HRTF_BLOCK);
        this.outBufR = new Float32Array(HRTF_BLOCK);
        this.outReadPos = 0;        // read cursor (0..255)
        this.outAvailable = 0;      // samples available to read

        // --- WASM pre-allocated pointers ---
        this.inputPtr = 0;
        this.outputPtr = 0;

        this.hold = false;     // silence output without consuming (buffering)
        this.useHrtf = true;   // false = speakers bypass mode
        this.muteBed = false;    // mute channels 0-7 (bed)
        this.muteHeight = false; // mute channels 8-15 (height objects)

        // --- Sync / diagnostics state ---
        this.playbackPts = -1;   // PTS of the sample currently being output
        this.underruns = 0;      // starved process() calls after playback began
        this.dropped = 0;        // chunks dropped on overflow
        this.wasPlaying = false;
        this.gainRamp = 1;       // 0..1, ramps up after discontinuities
        this.busyMs = 0;         // time spent inside halo_process
        this.statCounter = 0;

        this.port.onmessage = (e) => this.handleMessage(e.data);
    }

    handleMessage(msg) {
        switch (msg.type) {
            case 'init-wasm':
                this.initWasm(msg.wasm, msg.exportMap || {}, msg.importMap || {});
                break;
            case 'load-sofa':
                this.loadSofa(msg.data);
                break;
            case 'audio':
                this.queueAudio(msg.buffer);
                break;
            case 'set-hold':
                this.hold = !!msg.hold;
                if (!this.hold) this.gainRamp = 0;  // fade back in on resume
                break;
            case 'flush':
                this.audioQueue = [];
                this.accumPos = 0;
                this.outAvailable = 0;
                this.outReadPos = 0;
                this.playbackPts = -1;
                this.gainRamp = 0;
                this.wasPlaying = false;
                break;
            case 'audio-info': {
                const newCh = msg.info.channels || 2;
                if (newCh !== this.numChannels) {
                    this.numChannels = newCh;
                    this.accumBuf = new Float32Array(HRTF_BLOCK * this.numChannels);
                    this.accumPos = 0;
                    this.outAvailable = 0;
                    if (this.wasmReady && this.enginePtr) {
                        this.rebuildEngine();
                    }
                }
                break;
            }
            case 'set-layout':
                if (this.wasmReady && this.enginePtr)
                    this.fn.setLayout(this.enginePtr, msg.layout);
                break;
            case 'set-room-preset':
                if (this.wasmReady && this.enginePtr)
                    this.fn.setRoomPreset(this.enginePtr, msg.preset);
                break;
            case 'set-volume':
                this.volume = msg.volume;
                break;
            case 'set-output-mode':
                this.useHrtf = (msg.mode === 'headphones');
                break;
            case 'set-channel-mute':
                if (msg.group === 'bed') this.muteBed = msg.muted;
                if (msg.group === 'height') this.muteHeight = msg.muted;
                break;
        }
    }

    async initWasm(wasmBytes, exportMap, importMap) {
        try {
            const self = this;
            const module = await WebAssembly.compile(wasmBytes);

            // Build the import object dynamically: give every required import
            // a stub, with real implementations where semantics matter.
            const impls = {
                emscripten_resize_heap: (requestedSize) => {
                    try {
                        const pages = Math.ceil((requestedSize - self.memory.buffer.byteLength) / 65536);
                        if (pages > 0) self.memory.grow(pages);
                        return 1;
                    } catch (e) { return 0; }
                },
                assert_fail: () => { throw new Error('wasm assertion failed'); },
                abort_js: () => { throw new Error('wasm abort'); },
                emscripten_date_now: () => Date.now(),
            };
            const importObject = {};
            for (const imp of WebAssembly.Module.imports(module)) {
                importObject[imp.module] = importObject[imp.module] || {};
                if (imp.kind !== 'function') continue;
                const real = (importMap[imp.name] || imp.name).replace(/^_+/, '');
                importObject[imp.module][imp.name] = impls[real] || (() => 0);
            }

            const instance = await WebAssembly.instantiate(module, importObject);
            this.exports = instance.exports;

            // Resolve exports: unminified name first, then glue-derived map
            const ex = this.exports;
            const pick = (real) => ex[real] || ex['_' + real] || (exportMap[real] ? ex[exportMap[real]] : undefined);
            this.memory = ex.memory instanceof WebAssembly.Memory
                ? ex.memory
                : Object.values(ex).find(v => v instanceof WebAssembly.Memory);
            this.fn = {
                ctors: pick('wasm_call_ctors') || pick('__wasm_call_ctors'),
                create: pick('halo_create'),
                destroy: pick('halo_destroy'),
                loadSofa: pick('halo_load_sofa'),
                setLayout: pick('halo_set_layout'),
                setRoomPreset: pick('halo_set_room_preset'),
                process: pick('halo_process'),
                malloc: pick('malloc'),
                free: pick('free'),
            };
            for (const name of ['create', 'destroy', 'loadSofa', 'process', 'malloc', 'free']) {
                if (!this.fn[name]) throw new Error('unresolved wasm export: ' + name);
            }
            if (!this.memory) throw new Error('wasm memory export not found');

            // Runtime init (__wasm_call_ctors)
            if (this.fn.ctors) this.fn.ctors();

            this.enginePtr = this.fn.create(sampleRate, this.numChannels);
            if (!this.enginePtr) throw new Error('halo_create returned null');

            // Allocate WASM buffers for 256-sample blocks
            const inputBytes = HRTF_BLOCK * this.numChannels * 4;
            const outputBytes = HRTF_BLOCK * 2 * 4;
            this.inputPtr = this.fn.malloc(inputBytes);
            this.outputPtr = this.fn.malloc(outputBytes);

            // Init accumulation buffer
            this.accumBuf = new Float32Array(HRTF_BLOCK * this.numChannels);
            this.accumPos = 0;

            this.wasmReady = true;
            this.port.postMessage({ type: 'ready' });
        } catch (err) {
            this.port.postMessage({ type: 'error', message: err.toString() });
        }
    }

    rebuildEngine() {
        if (!this.fn) return;
        // Free old buffers and engine
        if (this.inputPtr) this.fn.free(this.inputPtr);
        if (this.outputPtr) this.fn.free(this.outputPtr);
        if (this.enginePtr) this.fn.destroy(this.enginePtr);

        this.enginePtr = this.fn.create(sampleRate, this.numChannels);
        const inputBytes = HRTF_BLOCK * this.numChannels * 4;
        const outputBytes = HRTF_BLOCK * 2 * 4;
        this.inputPtr = this.fn.malloc(inputBytes);
        this.outputPtr = this.fn.malloc(outputBytes);
    }

    loadSofa(sofaData) {
        if (!this.wasmReady || !this.enginePtr) return;
        const size = sofaData.byteLength;
        const ptr = this.fn.malloc(size);
        if (!ptr) return;
        new Uint8Array(this.memory.buffer).set(new Uint8Array(sofaData), ptr);
        this.fn.loadSofa(this.enginePtr, ptr, size);
        this.fn.free(ptr);
    }

    queueAudio(buffer) {
        const pcmBytes = buffer.byteLength - 8;
        if (pcmBytes <= 0) return;
        const numFloats = pcmBytes / 4;

        // PTS travels in the first 8 bytes (Float64 BE)
        const pts = new DataView(buffer).getFloat64(0, false);

        // Copy data (buffer will be detached)
        const pcmData = new Float32Array(numFloats);
        pcmData.set(new Float32Array(buffer, 8));

        this.audioQueue.push({ data: pcmData, readPos: 0, pts });

        // Cap queue (~5s at 48kHz). Latency is irrelevant here — the video
        // is slaved to the audio clock — so a deep buffer is pure underrun
        // protection (16ch × 5s ≈ 15 MB, trivial). Overflow only happens on
        // real server/DAC clock drift; drop oldest and fade back in so the
        // discontinuity doesn't click.
        while (this.audioQueue.length > 940) {
            this.audioQueue.shift();
            this.dropped++;
            this.gainRamp = 0;
        }
    }

    /**
     * Pull N interleaved samples from the audio queue into the accumulation buffer.
     * Returns number of samples actually pulled.
     */
    pullFromQueue(count) {
        const nch = this.numChannels;
        let pulled = 0;

        // Anchor the playback PTS to the queue head before consuming
        if (this.audioQueue.length > 0) {
            const head = this.audioQueue[0];
            this.playbackPts = head.pts + head.readPos / sampleRate;
        }

        while (pulled < count && this.audioQueue.length > 0) {
            const chunk = this.audioQueue[0];
            const chunkTotalFloats = chunk.data.length;
            const chunkTotalSamples = Math.floor(chunkTotalFloats / nch);
            const chunkRemaining = chunkTotalSamples - chunk.readPos;

            if (chunkRemaining <= 0) {
                this.audioQueue.shift();
                continue;
            }

            const toCopy = Math.min(count - pulled, chunkRemaining);
            const srcOffset = chunk.readPos * nch;
            const dstOffset = (this.accumPos + pulled) * nch;

            this.accumBuf.set(
                chunk.data.subarray(srcOffset, srcOffset + toCopy * nch),
                dstOffset
            );

            chunk.readPos += toCopy;
            pulled += toCopy;

            if (chunk.readPos >= chunkTotalSamples) {
                this.audioQueue.shift();
            }
        }

        return pulled;
    }

    /**
     * Zero-out muted channels in the accumulation buffer.
     * Bed = channels 0..7 (7.1 surround bed)
     * Height/Objects = channels 8..15 (Atmos spatial objects)
     * Data is interleaved: [s0ch0, s0ch1, ..., s0chN, s1ch0, ...]
     */
    applyChannelMute() {
        const nch = this.numChannels;
        for (let i = 0; i < HRTF_BLOCK; i++) {
            const base = i * nch;
            if (this.muteBed) {
                const bedEnd = Math.min(8, nch);
                for (let ch = 0; ch < bedEnd; ch++) {
                    this.accumBuf[base + ch] = 0;
                }
            }
            if (this.muteHeight) {
                // Mute object/height channels 8..15
                const objStart = Math.min(8, nch);
                for (let ch = objStart; ch < nch; ch++) {
                    this.accumBuf[base + ch] = 0;
                }
            }
        }
    }

    /**
     * Run halo_process on the full 256-sample accumulation buffer.
     * Stores stereo result in outBufL/outBufR.
     */
    runHrtf() {
        const nch = this.numChannels;
        const heapF32 = new Float32Array(this.memory.buffer);

        // Copy accumBuf → WASM input
        const inIdx = this.inputPtr >> 2;
        heapF32.set(this.accumBuf.subarray(0, HRTF_BLOCK * nch), inIdx);

        const t0 = Date.now();
        this.fn.process(this.enginePtr, this.inputPtr, this.outputPtr, HRTF_BLOCK);
        this.busyMs += Date.now() - t0;

        // Read interleaved stereo output → split to L/R
        const outIdx = this.outputPtr >> 2;
        for (let i = 0; i < HRTF_BLOCK; i++) {
            this.outBufL[i] = heapF32[outIdx + i * 2];
            this.outBufR[i] = heapF32[outIdx + i * 2 + 1];
        }

        this.outReadPos = 0;
        this.outAvailable = HRTF_BLOCK;
    }

    process(inputs, outputs, parameters) {
        const output = outputs[0];
        if (!output || output.length < 2) return true;

        const outL = output[0];
        const outR = output[1];

        // Held (video buffering / paused): silence without consuming queue
        if (this.hold) {
            outL.fill(0);
            outR.fill(0);
            this.wasPlaying = false;
            this.maybePostStats();
            return true;
        }

        let written = 0;

        while (written < RENDER_QUANTUM) {
            // 1) Drain any available processed output
            if (this.outAvailable > 0) {
                const toDrain = Math.min(this.outAvailable, RENDER_QUANTUM - written);
                const rp = this.outReadPos;
                for (let i = 0; i < toDrain; i++) {
                    // Short fade-in after any discontinuity (underrun/drop/seek)
                    if (this.gainRamp < 1) this.gainRamp = Math.min(1, this.gainRamp + 1 / 256);
                    const g = this.volume * this.gainRamp;
                    outL[written + i] = this.outBufL[rp + i] * g;
                    outR[written + i] = this.outBufR[rp + i] * g;
                }
                this.outReadPos += toDrain;
                this.outAvailable -= toDrain;
                written += toDrain;
                continue;
            }

            // 2) No output available — try to fill accumulation buffer and process
            if (!this.accumBuf) break;

            if (this.useHrtf && this.wasmReady && this.enginePtr) {
                // HRTF mode: accumulate full 256-sample block, then process
                const needed = HRTF_BLOCK - this.accumPos;
                const pulled = this.pullFromQueue(needed);
                this.accumPos += pulled;

                if (this.accumPos >= HRTF_BLOCK) {
                    // Apply channel muting for debug
                    if (this.muteBed || this.muteHeight) {
                        this.applyChannelMute();
                    }
                    this.runHrtf();
                    this.accumPos = 0;
                } else {
                    break;
                }
            } else {
                // Speakers bypass: simple stereo downmix, no accumulation needed
                const toPull = Math.min(RENDER_QUANTUM - written, HRTF_BLOCK);
                this.accumPos = 0;
                const pulled = this.pullFromQueue(toPull);
                if (pulled === 0) break;

                const nch = this.numChannels;
                for (let i = 0; i < pulled; i++) {
                    const base = i * nch;
                    const l = this.accumBuf[base] || 0;
                    const r = nch > 1 ? (this.accumBuf[base + 1] || 0) : l;
                    const c = nch > 2 ? (this.accumBuf[base + 2] || 0) * 0.707 : 0;
                    outL[written + i] = (l + c) * this.volume;
                    outR[written + i] = (r + c) * this.volume;
                }
                written += pulled;
                continue;
            }
        }

        // Zero-fill remainder
        for (let i = written; i < RENDER_QUANTUM; i++) {
            outL[i] = 0;
            outR[i] = 0;
        }

        // Underrun accounting: we were playing but this quantum starved.
        // Fade back in when data returns so the gap edge doesn't click.
        if (this.wasPlaying && written < RENDER_QUANTUM) {
            this.underruns++;
            this.gainRamp = 0;
        }
        this.wasPlaying = written === RENDER_QUANTUM;

        this.maybePostStats();

        return true;
    }

    /* Report sync/diagnostic stats every ~32 quanta (~85ms @48k) */
    maybePostStats() {
        if (++this.statCounter < 32) return;
        this.statCounter = 0;
        const windowMs = 32 * RENDER_QUANTUM / sampleRate * 1000;
        // headPts: PTS of the next sample that WILL play. Unlike playbackPts
        // (anchored on consumption) it stays exact while the worklet is held
        // or right after a flush — the realign-after-seek path needs that.
        const head = this.audioQueue[0];
        this.port.postMessage({
            type: 'stats',
            pts: this.playbackPts,
            headPts: head ? head.pts + head.readPos / sampleRate : this.playbackPts,
            queued: this.audioQueue.length,
            underruns: this.underruns,
            dropped: this.dropped,
            busyPct: Math.round(this.busyMs / windowMs * 100),
        });
        this.busyMs = 0;
    }
}

registerProcessor('hrtf-processor', HrtfProcessor);
