// HaloSound HRTF AudioWorklet Processor
//
// CRITICAL: halo_process() requires exactly 256 samples (HRTF_BLOCK_SIZE).
// Web Audio calls process() with 128-sample render quanta.
// We accumulate 256 input samples before calling WASM, then drain output
// over two process() calls.
//
// WASM export mapping (Emscripten 5.0.1 minified):
//   b = memory, c = __wasm_call_ctors
//   d = halo_create, e = halo_destroy, f = free
//   g = halo_load_sofa, h = halo_set_layout, i = halo_set_speaker_pos
//   j = halo_set_room, k = halo_set_room_preset, l = halo_process
//   m = halo_get_num_channels, n = malloc

const HRTF_BLOCK = 256;   // Must match HRTF_BLOCK_SIZE in C code
const RENDER_QUANTUM = 128; // Web Audio standard

class HrtfProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.exports = null;
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

        this.useHrtf = true;   // false = speakers bypass mode
        this.muteBed = false;    // mute channels 0-7 (bed)
        this.muteHeight = false; // mute channels 8-11 (height objects)

        this.port.onmessage = (e) => this.handleMessage(e.data);
    }

    handleMessage(msg) {
        switch (msg.type) {
            case 'init-wasm':
                this.initWasm(msg.wasm);
                break;
            case 'load-sofa':
                this.loadSofa(msg.data);
                break;
            case 'audio':
                this.queueAudio(msg.buffer);
                break;
            case 'flush':
                this.audioQueue = [];
                this.accumPos = 0;
                this.outAvailable = 0;
                this.outReadPos = 0;
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
                    this.exports.h(this.enginePtr, msg.layout);
                break;
            case 'set-room-preset':
                if (this.wasmReady && this.enginePtr)
                    this.exports.k(this.enginePtr, msg.preset);
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

    async initWasm(wasmBytes) {
        try {
            const self = this;
            const importObject = {
                a: {
                    a: (requestedSize) => {
                        // _emscripten_resize_heap
                        try {
                            const pages = Math.ceil((requestedSize - self.memory.buffer.byteLength) / 65536);
                            if (pages > 0) self.memory.grow(pages);
                            return 1;
                        } catch (e) { return 0; }
                    }
                }
            };

            const { instance } = await WebAssembly.instantiate(wasmBytes, importObject);
            this.exports = instance.exports;
            this.memory = instance.exports.b;  // b = memory

            // Runtime init (__wasm_call_ctors)
            if (this.exports.c) this.exports.c();

            // Create engine (d = halo_create)
            this.enginePtr = this.exports.d(sampleRate, this.numChannels);
            if (!this.enginePtr) throw new Error('halo_create returned null');

            // Allocate WASM buffers for 256-sample blocks (n = malloc)
            const inputBytes = HRTF_BLOCK * this.numChannels * 4;
            const outputBytes = HRTF_BLOCK * 2 * 4;
            this.inputPtr = this.exports.n(inputBytes);
            this.outputPtr = this.exports.n(outputBytes);

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
        if (!this.exports) return;
        // Free old buffers and engine (f = free, e = halo_destroy)
        if (this.inputPtr) this.exports.f(this.inputPtr);
        if (this.outputPtr) this.exports.f(this.outputPtr);
        if (this.enginePtr) this.exports.e(this.enginePtr);

        // d = halo_create, n = malloc
        this.enginePtr = this.exports.d(sampleRate, this.numChannels);
        const inputBytes = HRTF_BLOCK * this.numChannels * 4;
        const outputBytes = HRTF_BLOCK * 2 * 4;
        this.inputPtr = this.exports.n(inputBytes);
        this.outputPtr = this.exports.n(outputBytes);
    }

    loadSofa(sofaData) {
        if (!this.wasmReady || !this.enginePtr) return;
        const size = sofaData.byteLength;
        const ptr = this.exports.n(size);    // n = malloc
        if (!ptr) return;
        new Uint8Array(this.memory.buffer).set(new Uint8Array(sofaData), ptr);
        this.exports.g(this.enginePtr, ptr, size);  // g = halo_load_sofa
        this.exports.f(ptr);                        // f = free
    }

    queueAudio(buffer) {
        const pcmBytes = buffer.byteLength - 8;
        if (pcmBytes <= 0) return;
        const numFloats = pcmBytes / 4;

        // Copy data (buffer will be detached)
        const pcmData = new Float32Array(numFloats);
        pcmData.set(new Float32Array(buffer, 8));

        this.audioQueue.push({ data: pcmData, readPos: 0 });

        // Cap queue (~250ms at 48kHz)
        while (this.audioQueue.length > 48) {
            this.audioQueue.shift();
        }
    }

    /**
     * Pull N interleaved samples from the audio queue into the accumulation buffer.
     * Returns number of samples actually pulled.
     */
    pullFromQueue(count) {
        const nch = this.numChannels;
        let pulled = 0;

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

        // Process (l = halo_process)
        this.exports.l(this.enginePtr, this.inputPtr, this.outputPtr, HRTF_BLOCK);

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
        let written = 0;

        while (written < RENDER_QUANTUM) {
            // 1) Drain any available processed output
            if (this.outAvailable > 0) {
                const toDrain = Math.min(this.outAvailable, RENDER_QUANTUM - written);
                const rp = this.outReadPos;
                for (let i = 0; i < toDrain; i++) {
                    outL[written + i] = this.outBufL[rp + i] * this.volume;
                    outR[written + i] = this.outBufR[rp + i] * this.volume;
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

        return true;
    }
}

registerProcessor('hrtf-processor', HrtfProcessor);
