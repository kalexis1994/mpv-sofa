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

/* The LG's ethernet port is 100 Mbps (~94 effective after overhead). Files
 * whose average video bitrate exceeds this can never stream with -c:v copy
 * (VBR peaks starve the TV forever), so they get re-encoded with NVENC. */
const MAX_COPY_BPS = 80e6;
const NVENC_ARGS = [
    '-c:v', 'hevc_nvenc', '-preset', 'p4', '-tune', 'hq',
    '-rc', 'vbr', '-b:v', '40M', '-maxrate', '55M', '-bufsize', '110M',
    '-spatial-aq', '1',
    '-profile:v', 'main10', '-pix_fmt', 'p010le',
    '-g', '48', '-forced-idr', '1',   // ~2s GOP at 24fps so hls_time 2 can cut
                                      // (force_key_frames is ignored by nvenc)
];

/* Average container bitrate (bps), cached on the file entry. */
function probeBitrate(file, cb) {
    if (file._bitrate !== undefined) return cb(file._bitrate);
    execFile(config.FFPROBE_PATH, [
        '-v', 'quiet',
        '-show_entries', 'format=bit_rate',
        '-print_format', 'json',
        file.path
    ], (err, stdout) => {
        let b = 0;
        if (!err) { try { b = parseInt(JSON.parse(stdout).format.bit_rate) || 0; } catch (e) {} }
        file._bitrate = b;
        cb(b);
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
        this.dir = path.join(os.tmpdir(), 'halosound-hls', this.id);
        this.procs = [];
        this.stopped = false;
        this.verbose = opts.verbose;
    }

    playlistPath() { return path.join(this.dir, 'out.m3u8'); }

    start() {
        fs.mkdirSync(this.dir, { recursive: true });

        const ssArgs = this.base > 0 ? ['-ss', String(this.base)] : [];

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
        const render = spawn(RENDER_EXE, [
            '--sofa', this.sofaPath,
            '--channels', String(this.channels),
            '--room', String(this.room),
        ], { stdio: ['pipe', 'pipe', 'pipe'] });

        // 3) mux: video (copied, or NVENC re-encode when the source bitrate
        //    exceeds what the TV's 100 Mbps port can carry) + AAC binaural
        //    → growing HLS (fMP4)
        const ffM = spawn(config.FFMPEG_PATH, [
            '-v', 'error', '-y',
            ...(this.transcode ? ['-hwaccel', 'cuda'] : []),
            ...ssArgs,
            '-i', this.file.path,
            '-f', 'f32le', '-ar', '48000', '-ac', '2', '-i', 'pipe:0',
            '-map', '0:v:0',
            ...(this.transcode ? NVENC_ARGS : ['-c:v', 'copy']),
            '-map', '1:a', '-c:a', 'aac', '-b:a', '256k',
            '-f', 'hls',
            '-hls_time', '2',
            '-hls_segment_type', 'fmp4',
            '-hls_playlist_type', 'event',
            '-hls_list_size', '0',
            '-hls_fmp4_init_filename', 'init.mp4',
            this.playlistPath()
        ], { stdio: ['pipe', 'ignore', 'pipe'], cwd: this.dir });

        ffA.stdout.pipe(render.stdin);
        render.stdout.pipe(ffM.stdin);

        const tag = (p, name) => {
            p.stderr.on('data', d => { if (this.verbose) console.error(`[hls:${name}]`, d.toString().trim().slice(0, 300)); });
            p.on('error', e => console.error(`[hls:${name}] spawn error`, e.message));
            // Broken-pipe errors are expected when the chain tears down.
            for (const s of [p.stdin, p.stdout]) if (s) s.on('error', () => {});
        };
        tag(ffA, 'audio'); tag(render, 'render'); tag(ffM, 'mux');

        this.procs = [ffA, render, ffM];
        if (this.verbose) {
            console.log(`[hls] session ${this.id}: ${path.basename(this.file.path)} ` +
                        `a=#${this.audioStreamIndex} ${this.channels}ch room=${this.room} base=${this.base.toFixed(2)}s ` +
                        `v=${this.transcode ? 'nvenc' : 'copy'}`);
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
        const bitrate = await new Promise(res => probeBitrate(file, res));
        const transcode = bitrate > MAX_COPY_BPS;
        if (transcode && options.verbose) {
            console.log(`[hls] ${path.basename(file.path)}: ${(bitrate / 1e6).toFixed(1)} Mbps ` +
                        `> ${(MAX_COPY_BPS / 1e6).toFixed(0)} Mbps cap → NVENC re-encode`);
        }
        // Even when re-encoding, both pipelines start at a source keyframe so
        // audio (exact seek) and video (keyframe-snapped -ss) stay aligned.
        const base = await new Promise(res => findKeyframeBefore(file.path, t, res));

        const session = new HlsSession({
            file,
            audioStreamIndex: opts.audioStreamIndex,
            channels: opts.channels,
            sofaPath: opts.sofaPath || DEFAULT_SOFA,
            room: Number.isFinite(opts.room) ? opts.room : 1,
            base,
            transcode,
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
