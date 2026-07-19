const { spawn, execFile } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');
const config = require('./config');

/*
 * HLS session manager — server-side binaural rendering ("TV-native" mode).
 *
 * Instead of splitting audio (WebSocket PCM) and video (HTTP) across two
 * transports with two clocks, a session produces ONE HLS stream the TV's
 * own player consumes: video copied as-is, audio decoded → rendered to
 * binaural stereo through halosound-render (the SAME DSP the web client
 * uses: HRTF + room presets + reverb + early reflections) → AAC.
 * The TV then handles A/V sync natively, including its internal audio
 * pipeline and Bluetooth latency — the entire two-clock problem class
 * disappears.
 *
 * Seek alignment: with -c:v copy the video can only start on a keyframe,
 * so the session start is snapped to the keyframe <= requested time and
 * BOTH pipelines start exactly there. The client learns the actual start
 * ("base") and maps its UI timeline accordingly.
 */

const RENDER_EXE = (() => {
    const candidates = [
        process.env.HALO_RENDER_PATH,
        path.resolve(__dirname, '../../../dist/halosound-render.exe'),   // packaged
        path.join(os.homedir(), 'hrtf-build', 'halosound-render.exe'),   // dev build
    ].filter(Boolean);
    for (const c of candidates) { try { if (fs.existsSync(c)) return c; } catch (e) {} }
    return candidates[candidates.length - 1];
})();
const DEFAULT_SOFA = path.resolve(__dirname, '../../app/assets/hrtf/default.sofa');

/* truehdd (TrueHD/Atmos decoder) enables object-based binaural rendering:
 * instead of the 7.1 bed, the real Atmos objects + 3D trajectories are
 * decoded (DAMF) and spatialized individually by halosound-render. */
const TRUEHDD_EXE = (() => {
    const candidates = [
        process.env.HALO_TRUEHDD_PATH,
        path.resolve(__dirname, '../../../dist/truehdd.exe'),
        path.join(os.homedir(), 'hrtf-build', 'truehdd-bin', 'truehdd.exe'),
    ].filter(Boolean);
    for (const c of candidates) { try { if (fs.existsSync(c)) return c; } catch (e) {} }
    return null;
})();

/* Files whose average bitrate exceeds what the LAN link to the TV can carry
 * can never stream with -c:v copy (VBR peaks starve the TV forever), so they
 * get re-encoded with NVENC. The client measures its real throughput on
 * connect (/api/speedtest) and reports it per session as `bw`; without a
 * measurement we assume the LG's 100 Mbps port (~94 effective). */
const FALLBACK_BW = 94e6;

/* Copy is safe while the average leaves ~15% headroom for VBR peaks,
 * audio and HTTP overhead. */
function copyCap(bw) { return (bw || FALLBACK_BW) * 0.85; }

/* NVENC target scaled to the link: ~55% of throughput as average, ~70% as
 * ceiling. Clamped — below 20M 4K quality collapses; above 60M the gains
 * are invisible while GPU/disk cost keeps growing. */
function nvencArgs(bw) {
    const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
    const link = bw || FALLBACK_BW;
    const target = Math.round(clamp(link * 0.55, 20e6, 60e6));
    const maxr = Math.round(clamp(link * 0.70, 25e6, 75e6));
    return {
        target,
        args: [
            '-c:v', 'hevc_nvenc', '-preset', 'p4', '-tune', 'hq',
            '-rc', 'vbr', '-b:v', String(target),
            '-maxrate', String(maxr), '-bufsize', String(maxr * 2),
            '-spatial-aq', '1',
            '-profile:v', 'main10', '-pix_fmt', 'p010le',
            '-g', '48', '-forced-idr', '1',   // ~2s GOP at 24fps so hls_time 2 can cut
                                              // (force_key_frames is ignored by nvenc)
        ],
    };
}

/* Codec + channel count (+Atmos flag) of one audio stream (absolute index). */
function probeAudioStream(file, absIndex, cb) {
    execFile(config.FFPROBE_PATH, [
        '-v', 'quiet',
        '-select_streams', String(absIndex),
        '-show_entries', 'stream=codec_name,channels,profile',
        '-print_format', 'json',
        file.path
    ], (err, stdout) => {
        let codec = '', channels = 6, atmos = false;
        if (!err) {
            try {
                const s = (JSON.parse(stdout).streams || [])[0] || {};
                codec = s.codec_name || '';
                channels = s.channels || 6;
                atmos = /atmos/i.test(s.profile || '');
            } catch (e) {}
        }
        cb({ codec, channels, atmos });
    });
}

/* Last TrueHD major sync (ffprobe marks them as keyframes, one every
 * ~106.7ms) at or before `t`. truehdd can only start decoding there, so
 * the extraction starts exactly on one and the renderer trims the
 * difference to `t` — sample-exact A/V alignment. */
function findAudioSyncBefore(filePath, absIndex, t, cb) {
    if (t <= 0.2) return cb(0);
    execFile(config.FFPROBE_PATH, [
        '-v', 'quiet',
        '-select_streams', String(absIndex),
        '-show_packets',
        '-show_entries', 'packet=pts_time,flags',
        '-read_intervals', `${Math.max(0, t - 1)}%${t + 0.05}`,
        '-print_format', 'json',
        filePath
    ], { maxBuffer: 32 * 1024 * 1024 }, (err, stdout) => {
        if (err) return cb(Math.max(0, t - 0.25));
        try {
            const pkts = (JSON.parse(stdout).packets || [])
                .filter(p => (p.flags || '').includes('K') && parseFloat(p.pts_time) <= t)
                .map(p => parseFloat(p.pts_time));
            cb(pkts.length ? Math.max(...pkts) : Math.max(0, t - 0.25));
        } catch (e) {
            cb(Math.max(0, t - 0.25));
        }
    });
}

/* Average container bitrate (bps) + duration (s), cached on the file entry. */
function probeFormat(file, cb) {
    if (file._fmt) return cb(file._fmt);
    execFile(config.FFPROBE_PATH, [
        '-v', 'quiet',
        '-show_entries', 'format=bit_rate,duration',
        '-print_format', 'json',
        file.path
    ], (err, stdout) => {
        let fmt = { bitrate: 0, duration: 0 };
        if (!err) {
            try {
                const f = JSON.parse(stdout).format || {};
                fmt.bitrate = parseInt(f.bit_rate) || 0;
                fmt.duration = parseFloat(f.duration) || 0;
            } catch (e) {}
        }
        file._fmt = fmt;
        cb(fmt);
    });
}

/* Average video keyframe interval (s) around `t` — determines the segment
 * duration hls_time produces with -c:v copy (cuts on the first keyframe at
 * or after each 2s boundary). Cached on the file entry. */
function probeGopInterval(file, t, cb) {
    if (file._gop) return cb(file._gop);
    execFile(config.FFPROBE_PATH, [
        '-v', 'quiet',
        '-select_streams', 'v:0',
        '-show_packets',
        '-show_entries', 'packet=pts_time,flags',
        '-read_intervals', `${Math.max(0, t)}%${Math.max(0, t) + 60}`,
        '-print_format', 'json',
        file.path
    ], { maxBuffer: 64 * 1024 * 1024 }, (err, stdout) => {
        let gop = 2.0;
        if (!err) {
            try {
                const keys = (JSON.parse(stdout).packets || [])
                    .filter(p => (p.flags || '').includes('K'))
                    .map(p => parseFloat(p.pts_time))
                    .sort((a, b) => a - b);
                if (keys.length >= 3) gop = (keys[keys.length - 1] - keys[0]) / (keys.length - 1);
            } catch (e) {}
        }
        file._gop = gop;
        cb(gop);
    });
}

/* Find the last video keyframe at or before `t` (seconds). Reads only the
 * ~30s packet window before t, so it's fast even on 60GB files. */
function findKeyframeBefore(filePath, t, cb) {
    if (t <= 0.5) return cb(0);
    const from = Math.max(0, t - 30);
    execFile(config.FFPROBE_PATH, [
        '-v', 'quiet',
        '-select_streams', 'v:0',
        '-show_packets',
        '-show_entries', 'packet=pts_time,flags',
        '-read_intervals', `${from}%${t + 0.5}`,
        '-print_format', 'json',
        filePath
    ], { maxBuffer: 32 * 1024 * 1024 }, (err, stdout) => {
        if (err) return cb(Math.max(0, t - 10));   // fallback: generous rewind
        try {
            const pkts = (JSON.parse(stdout).packets || [])
                .filter(p => (p.flags || '').includes('K') && parseFloat(p.pts_time) <= t)
                .map(p => parseFloat(p.pts_time));
            cb(pkts.length ? Math.max(...pkts) : Math.max(0, t - 10));
        } catch (e) {
            cb(Math.max(0, t - 10));
        }
    });
}

class HlsSession {
    constructor(opts) {
        this.id = `${Date.now().toString(36)}-${Math.floor(Math.random() * 1e6).toString(36)}`;
        this.file = opts.file;
        this.audioStreamIndex = opts.audioStreamIndex;   // absolute stream index
        this.channels = opts.channels;
        this.sofaPath = opts.sofaPath;
        this.room = opts.room;
        this.base = opts.base;                            // keyframe-aligned start (s)
        this.transcode = !!opts.transcode;                // re-encode video (NVENC)
        this.bw = opts.bw || 0;                           // client-measured link (bps)
        this.passthrough = !!opts.passthrough;            // original audio, TV decodes
        this.audioCodec = opts.audioCodec || '';          // source codec (passthrough)
        this.audioChannels = opts.audioChannels || 6;
        this.objectAudio = !!opts.objectAudio;            // Atmos objects via truehdd
        this.audioSync = opts.audioSync || 0;             // TrueHD major sync <= base
        this.totalDuration = opts.totalDuration || 0;     // movie length (s)
        this.segEstimate = opts.segEstimate || 0;         // projected segment dur (s)
        this.audioDelay = opts.audioDelay || 0;           // shift audio later (s)
        this.dir = path.join(os.tmpdir(), 'halosound-hls', this.id);
        this.procs = [];
        this.stopped = false;
        this.verbose = opts.verbose;
    }

    playlistPath() { return path.join(this.dir, 'out.m3u8'); }

    start() {
        fs.mkdirSync(this.dir, { recursive: true });

        const ssArgs = this.base > 0 ? ['-ss', String(this.base)] : [];
        const videoArgs = [
            '-map', '0:v:0',
            ...(this.transcode ? nvencArgs(this.bw).args : ['-c:v', 'copy']),
        ];
        const hlsArgs = [
            '-f', 'hls',
            '-hls_time', '2',
            '-hls_segment_type', 'fmp4',
            '-hls_playlist_type', 'event',
            '-hls_list_size', '0',
            '-hls_fmp4_init_filename', 'init.mp4',
            this.playlistPath()
        ];
        const tag = (p, name) => {
            p.stderr.on('data', d => { if (this.verbose) console.error(`[hls:${name}]`, d.toString().trim().slice(0, 300)); });
            p.on('error', e => console.error(`[hls:${name}] spawn error`, e.message));
            // Broken-pipe errors are expected when the chain tears down.
            for (const s of [p.stdin, p.stdout]) if (s) s.on('error', () => {});
        };

        if (this.passthrough) {
            // Original audio, single process — the TV decodes it itself.
            // E-AC-3/AC-3 ride fMP4 untouched (Atmos JOC metadata survives);
            // TrueHD/DTS can't be carried in fMP4, so they become DD+ 5.1.
            // Audio delay pads real silence (adelay) — that requires the
            // decode path, so a delayed session re-encodes to DD+ even for
            // E-AC-3 sources (Atmos JOC is lost while a delay is set).
            const copyOk = (this.audioCodec === 'eac3' || this.audioCodec === 'ac3') &&
                           this.audioDelay <= 0;
            const delayMs = Math.round(this.audioDelay * 1000);
            const ffM = spawn(config.FFMPEG_PATH, [
                '-v', 'error', '-y',
                ...(this.transcode ? ['-hwaccel', 'cuda'] : []),
                ...ssArgs,
                '-i', this.file.path,
                ...videoArgs,
                '-map', `0:${this.audioStreamIndex}`,
                ...(copyOk ? ['-c:a', 'copy']
                           : ['-c:a', 'eac3', '-b:a', '640k',
                              ...(this.audioChannels > 6 ? ['-ac', '6'] : []),
                              ...(delayMs > 0 ? ['-af', `adelay=${delayMs}:all=1`] : [])]),
                ...hlsArgs
            ], { stdio: ['ignore', 'ignore', 'pipe'], cwd: this.dir });
            tag(ffM, 'mux');
            this.procs = [ffM];
            if (this.verbose) {
                console.log(`[hls] session ${this.id}: ${path.basename(this.file.path)} ` +
                            `a=#${this.audioStreamIndex} passthrough(${copyOk ? 'copy ' + this.audioCodec : this.audioCodec + '→eac3'}) ` +
                            `base=${this.base.toFixed(2)}s ` +
                            `v=${this.transcode ? 'nvenc@' + (nvencArgs(this.bw).target / 1e6).toFixed(0) + 'M' : 'copy'}` +
                            (this.bw ? ` link=${(this.bw / 1e6).toFixed(0)}Mbps` : ''));
            }
            return;
        }

        let audioProcs, render;
        if (this.objectAudio) {
            // 1) Object-based Atmos: raw TrueHD (cut exactly on a major
            //    sync) → truehdd → DAMF (beds+objects+3D metadata) → render
            //    spatializes each object; --skip trims sync→base for
            //    sample-exact alignment with the video.
            const sync = this.audioSync;
            const pre = Math.max(0, sync - 3);
            const damfPrefix = path.join(this.dir, 'damf');
            const ffX = spawn(config.FFMPEG_PATH, [
                '-v', 'fatal',
                ...(pre > 0 ? ['-ss', String(pre)] : []),
                '-i', this.file.path,
                '-ss', String(sync - pre),
                '-map', `0:${this.audioStreamIndex}`,
                '-c:a', 'copy',
                '-f', 'truehd', '-'
            ], { stdio: ['ignore', 'pipe', 'pipe'] });

            const thd = spawn(TRUEHDD_EXE, [
                'decode', '-',
                '--output-path', damfPrefix,
                '--loglevel', 'error',
            ], { stdio: ['pipe', 'ignore', 'pipe'] });
            ffX.stdout.pipe(thd.stdin);
            thd.on('close', () => {
                // Signals the renderer that the DAMF files stop growing.
                try { if (!this.stopped) fs.writeFileSync(damfPrefix + '.done', ''); } catch (e) {}
            });

            const skip = Math.max(0, Math.round((this.base - sync) * 48000));
            render = spawn(RENDER_EXE, [
                '--sofa', this.sofaPath,
                '--room', String(this.room),
                '--damf', damfPrefix,
                '--follow',
                '--skip', String(skip),
            ], { stdio: ['ignore', 'pipe', 'pipe'] });
            audioProcs = [[ffX, 'extract'], [thd, 'truehdd'], [render, 'render']];
        } else {
            // 1) audio: decode selected track to multichannel f32
            const ffA = spawn(config.FFMPEG_PATH, [
                '-v', 'error',
                ...ssArgs,
                '-i', this.file.path,
                '-map', `0:${this.audioStreamIndex}`,
                '-vn',
                '-acodec', 'pcm_f32le',
                '-ar', '48000',
                '-ac', String(this.channels),
                '-f', 'f32le', '-'
            ], { stdio: ['ignore', 'pipe', 'pipe'] });

            // 2) binaural render (same DSP as the TV client)
            render = spawn(RENDER_EXE, [
                '--sofa', this.sofaPath,
                '--channels', String(this.channels),
                '--room', String(this.room),
            ], { stdio: ['pipe', 'pipe', 'pipe'] });
            ffA.stdout.pipe(render.stdin);
            audioProcs = [[ffA, 'audio'], [render, 'render']];
        }

        // 3) mux: video (copied, or NVENC re-encode when the source bitrate
        //    exceeds what the TV's 100 Mbps port can carry) + AAC binaural
        //    → growing HLS (fMP4)
        const ffM = spawn(config.FFMPEG_PATH, [
            '-v', 'error', '-y',
            ...(this.transcode ? ['-hwaccel', 'cuda'] : []),
            ...ssArgs,
            '-i', this.file.path,
            '-f', 'f32le', '-ar', '48000', '-ac', '2', '-i', 'pipe:0',
            ...videoArgs,
            '-map', '1:a', '-c:a', 'aac', '-b:a', '256k',
            ...(this.audioDelay > 0
                ? ['-af', `adelay=${Math.round(this.audioDelay * 1000)}:all=1`] : []),
            ...hlsArgs
        ], { stdio: ['pipe', 'ignore', 'pipe'], cwd: this.dir });

        render.stdout.pipe(ffM.stdin);

        for (const [p, name] of audioProcs) tag(p, name);
        tag(ffM, 'mux');

        this.procs = [...audioProcs.map(([p]) => p), ffM];
        if (this.verbose) {
            const aDesc = this.objectAudio
                ? `objects(damf, skip=${((this.base - this.audioSync) * 1000).toFixed(0)}ms)`
                : `${this.channels}ch`;
            console.log(`[hls] session ${this.id}: ${path.basename(this.file.path)} ` +
                        `a=#${this.audioStreamIndex} ${aDesc} room=${this.room} base=${this.base.toFixed(2)}s ` +
                        `v=${this.transcode ? 'nvenc@' + (nvencArgs(this.bw).target / 1e6).toFixed(0) + 'M' : 'copy'}` +
                        (this.bw ? ` link=${(this.bw / 1e6).toFixed(0)}Mbps` : ''));
        }
    }

    /* Resolves when the playlist exists with at least `minSegs` segments. */
    waitReady(minSegs = 2, timeoutMs = 30000) {
        const start = Date.now();
        return new Promise((resolve, reject) => {
            const poll = () => {
                if (this.stopped) return reject(new Error('session stopped'));
                try {
                    const m3u8 = fs.readFileSync(this.playlistPath(), 'utf8');
                    if ((m3u8.match(/#EXTINF/g) || []).length >= minSegs) return resolve();
                } catch (e) { /* not yet */ }
                if (Date.now() - start > timeoutMs) return reject(new Error('HLS start timeout'));
                setTimeout(poll, 250);
            };
            poll();
        });
    }

    stop() {
        this.stopped = true;
        for (const p of this.procs) { try { p.kill('SIGTERM'); } catch (e) {} }
        this.procs = [];
        // Give handles a moment to release before deleting segment files.
        setTimeout(() => {
            fs.rm(this.dir, { recursive: true, force: true }, () => {});
        }, 1500);
    }
}

function createHlsManager(options = {}) {
    let active = null;   // single client (the TV) → single active session

    function stopAll() {
        if (active) { active.stop(); active = null; }
    }

    /* Sweep any orphaned session dirs from previous runs. */
    try {
        const root = path.join(os.tmpdir(), 'halosound-hls');
        for (const d of fs.readdirSync(root)) {
            fs.rm(path.join(root, d), { recursive: true, force: true }, () => {});
        }
    } catch (e) { /* none */ }

    async function startSession(file, opts) {
        // Keep the previous session alive until the new one is READY: the
        // TV may still be fetching the old playlist, and webOS's media
        // daemon reacts badly to streams dying under it (wedged pipelines
        // that survive app restarts).  Brief overlap of two transcodes is
        // the lesser evil.
        const previous = active;
        active = null;

        const t = Math.max(0, parseFloat(opts.t) || 0);
        const bw = Math.max(0, parseFloat(opts.bw) || 0);
        const audioDelay = Math.min(2, Math.max(0, (parseFloat(opts.delay) || 0) / 1000));
        const passthrough = opts.audioMode === 'original';
        const audioInfo = await new Promise(res => probeAudioStream(file, opts.audioStreamIndex, res));
        // TrueHD Atmos + truehdd available → object-based binaural render
        const objectAudio = !passthrough && !!TRUEHDD_EXE &&
            audioInfo.codec === 'truehd' && audioInfo.atmos;
        const fmt = await new Promise(res => probeFormat(file, res));
        const bitrate = fmt.bitrate;
        const transcode = bitrate > copyCap(bw);
        if (transcode && options.verbose) {
            console.log(`[hls] ${path.basename(file.path)}: ${(bitrate / 1e6).toFixed(1)} Mbps ` +
                        `> ${(copyCap(bw) / 1e6).toFixed(0)} Mbps cap → NVENC re-encode`);
        }
        // With -c:v copy the session can only start on a source keyframe
        // (audio decodes exactly, video snaps — both start together at the
        // keyframe). A re-encode decodes the video first, so it starts at
        // the exact requested time: seeks land where the user asked.
        const base = transcode
            ? t
            : await new Promise(res => findKeyframeBefore(file.path, t, res));
        // Object audio starts on a TrueHD major sync <= base; render trims
        // (base - sync) samples for exact alignment.
        const audioSync = objectAudio
            ? await new Promise(res => findAudioSyncBefore(file.path, opts.audioStreamIndex, base, res))
            : 0;

        const session = new HlsSession({
            file,
            audioStreamIndex: opts.audioStreamIndex,
            channels: opts.channels,
            sofaPath: opts.sofaPath || DEFAULT_SOFA,
            room: Number.isFinite(opts.room) ? opts.room : 1,
            base,
            transcode,
            bw,
            passthrough,
            audioCodec: audioInfo.codec,
            audioChannels: audioInfo.channels,
            objectAudio,
            audioSync,
            totalDuration: fmt.duration,
            audioDelay,
            // Segment duration the muxer will actually produce: exact 2s
            // GOPs when re-encoding, first-keyframe-after-2s with copy.
            segEstimate: transcode ? 2.002 : await new Promise(res =>
                probeGopInterval(file, t, gop => {
                    res(Math.max(1, Math.ceil(2.002 / gop - 1e-6)) * gop);
                })),
            verbose: options.verbose,
        });
        session.start();
        active = session;
        try {
            await session.waitReady();
        } catch (e) {
            session.stop();
            if (active === session) active = previous;   // keep serving the old one
            throw e;
        }
        if (active !== session) { session.stop(); throw new Error('superseded'); }
        if (previous) previous.stop();
        return session;
    }

    function getActive() { return active; }

    return { startSession, stopAll, getActive, renderExe: RENDER_EXE, defaultSofa: DEFAULT_SOFA };
}

module.exports = { createHlsManager };
