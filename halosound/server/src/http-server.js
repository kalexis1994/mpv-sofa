const express = require('express');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { execFile, spawn } = require('child_process');
const crypto = require('crypto');
const config = require('./config');
const { createHlsManager } = require('./hls-session');

/**
 * Create and configure the HTTP server.
 * @param {Array} files - Scanned file list from file-scanner
 * @param {Object} options - { verbose: boolean, appDir: string }
 * @returns {express.Application}
 */
function createHttpServer(files, options = {}) {
    const app = express();

    /* CORS headers for cross-origin access from TV app */
    app.use((req, res, next) => {
        res.header('Access-Control-Allow-Origin', '*');
        res.header('Access-Control-Allow-Methods', 'GET, OPTIONS');
        res.header('Access-Control-Allow-Headers', 'Range, Content-Type');
        res.header('Access-Control-Expose-Headers', 'Content-Range, Content-Length, Accept-Ranges');
        if (req.method === 'OPTIONS') return res.sendStatus(200);
        next();
    });

    /* Serve the web app UI (static files from app/ directory) */
    const appDir = options.appDir || path.resolve(__dirname, '../../app');
    app.use('/app', express.static(appDir, {
        setHeaders: (res, filePath) => {
            /* Correct MIME types for WASM and JS modules */
            if (filePath.endsWith('.wasm')) {
                res.set('Content-Type', 'application/wasm');
            } else if (filePath.endsWith('.js')) {
                res.set('Content-Type', 'application/javascript');
            }
        }
    }));

    /* Redirect root to the app UI */
    app.get('/', (req, res) => {
        res.redirect('/app/');
    });

    /* List available media files (rescan so new files appear without restart) */
    app.get('/api/files', (req, res) => {
        if (options.rescan) {
            try { options.rescan(); } catch (e) { /* keep serving the old list */ }
        }
        res.json(files.map(f => ({
            id: f.id,
            name: f.name,
            size: f.size,
            ext: f.ext
        })));
    });

    /*
     * Browse the library as a folder tree.  ?dir=<relative path> lists the
     * immediate sub-folders (only those that contain a video somewhere below,
     * so dead-end folders never show) and the video files directly inside.
     * The tree is derived from the already-scanned flat file list — no extra
     * filesystem walk, and a bad `dir` simply matches nothing (no traversal).
     */
    app.get('/api/browse', (req, res) => {
        if (options.rescan) {
            try { options.rescan(); } catch (e) { /* keep serving the old list */ }
        }
        const mediaDir = options.mediaDir || '';
        const relParts = String(req.query.dir || '')
            .split(/[\\/]/).filter(p => p && p !== '.' && p !== '..');

        const folders = new Set();
        const filesOut = [];
        for (const f of files) {
            const rel = mediaDir ? path.relative(mediaDir, f.path) : f.name;
            if (rel.startsWith('..')) continue;
            const parts = rel.split(/[\\/]/);
            // must sit under the requested dir
            let under = parts.length > relParts.length;
            for (let i = 0; under && i < relParts.length; i++) {
                if (parts[i] !== relParts[i]) under = false;
            }
            if (!under) continue;
            const rest = parts.slice(relParts.length);
            if (rest.length === 1) {
                filesOut.push({ id: f.id, name: rest[0], size: f.size, ext: f.ext });
            } else {
                folders.add(rest[0]);
            }
        }

        const coll = (a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' });
        res.json({
            dir: relParts.join('/'),
            folders: [...folders].sort(coll),
            files: filesOut.sort((a, b) => coll(a.name, b.name)),
        });
    });

    /* Get file info via ffprobe */
    app.get('/api/files/:id/info', (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).json({ error: 'File not found' });

        const args = [
            '-v', 'quiet',
            '-print_format', 'json',
            '-show_streams',
            '-show_format',
            file.path
        ];

        execFile(config.FFPROBE_PATH, args, { maxBuffer: 1024 * 1024 }, (err, stdout) => {
            if (err) {
                return res.status(500).json({ error: 'ffprobe failed', details: err.message });
            }

            try {
                const probe = JSON.parse(stdout);
                const videoStream = (probe.streams || []).find(s => s.codec_type === 'video');
                const audioStream = (probe.streams || []).find(s => s.codec_type === 'audio');

                res.json({
                    id: file.id,
                    name: file.name,
                    size: file.size,
                    duration: probe.format ? parseFloat(probe.format.duration) : 0,
                    video: videoStream ? {
                        codec: videoStream.codec_name,
                        width: videoStream.width,
                        height: videoStream.height,
                        fps: videoStream.r_frame_rate
                    } : null,
                    audio: audioStream ? {
                        codec: audioStream.codec_name,
                        channels: audioStream.channels,
                        sampleRate: parseInt(audioStream.sample_rate),
                        layout: audioStream.channel_layout || `${audioStream.channels}ch`,
                        bitRate: audioStream.bit_rate ? parseInt(audioStream.bit_rate) : 0
                    } : null
                });
            } catch (e) {
                res.status(500).json({ error: 'Failed to parse ffprobe output' });
            }
        });
    });

    /* List all audio and subtitle tracks for a file */
    app.get('/api/files/:id/tracks', (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).json({ error: 'File not found' });

        const args = [
            '-v', 'quiet',
            '-print_format', 'json',
            '-show_streams',
            '-show_format',
            file.path
        ];

        execFile(config.FFPROBE_PATH, args, { maxBuffer: 2 * 1024 * 1024 }, (err, stdout) => {
            if (err) return res.status(500).json({ error: 'ffprobe failed' });

            try {
                const probe = JSON.parse(stdout);
                const streams = probe.streams || [];
                const duration = probe.format ? parseFloat(probe.format.duration) : 0;

                const codecNames = {
                    'truehd': 'Dolby TrueHD',
                    'eac3': 'Dolby Digital Plus',
                    'ac3': 'Dolby Digital',
                    'dts': 'DTS',
                    'aac': 'AAC',
                    'flac': 'FLAC',
                    'vorbis': 'Vorbis',
                    'opus': 'Opus',
                    'mp3': 'MP3',
                    'subrip': 'SRT',
                    'ass': 'ASS',
                    'ssa': 'SSA',
                    'hdmv_pgs_subtitle': 'PGS',
                    'dvd_subtitle': 'VobSub',
                    'webvtt': 'WebVTT',
                    'mov_text': 'Timed Text',
                };

                const langNames = {
                    'eng': 'English', 'spa': 'Spanish', 'fre': 'French', 'fra': 'French',
                    'ger': 'German', 'deu': 'German', 'ita': 'Italian', 'por': 'Portuguese',
                    'rus': 'Russian', 'jpn': 'Japanese', 'kor': 'Korean', 'chi': 'Chinese',
                    'zho': 'Chinese', 'ara': 'Arabic', 'hin': 'Hindi', 'ukr': 'Ukrainian',
                    'pol': 'Polish', 'nld': 'Dutch', 'dut': 'Dutch', 'swe': 'Swedish',
                    'nor': 'Norwegian', 'dan': 'Danish', 'fin': 'Finnish', 'tur': 'Turkish',
                    'heb': 'Hebrew', 'tha': 'Thai', 'vie': 'Vietnamese', 'hun': 'Hungarian',
                    'cze': 'Czech', 'ces': 'Czech', 'rum': 'Romanian', 'ron': 'Romanian',
                    'gre': 'Greek', 'ell': 'Greek', 'bul': 'Bulgarian', 'hrv': 'Croatian',
                    'lat': 'Latin', 'und': 'Unknown',
                };

                const audioTracks = [];
                const subtitleTracks = [];

                for (const s of streams) {
                    const lang = (s.tags && s.tags.language) || '';
                    const title = (s.tags && s.tags.title) || '';
                    const langName = langNames[lang] || lang.toUpperCase() || '';

                    if (s.codec_type === 'audio') {
                        const codec = codecNames[s.codec_name] || s.codec_name;
                        const layout = s.channel_layout || `${s.channels}ch`;
                        /* DTS-HD MA detection */
                        let codecLabel = codec;
                        if (s.codec_name === 'dts' && s.profile) {
                            codecLabel = s.profile.includes('MA') ? 'DTS-HD MA'
                                       : s.profile.includes('HRA') ? 'DTS-HD HRA'
                                       : s.profile.includes('ES') ? 'DTS-ES' : codec;
                        }

                        audioTracks.push({
                            index: s.index,
                            codec: s.codec_name,
                            codecLabel,
                            channels: s.channels || 2,
                            layout,
                            sampleRate: parseInt(s.sample_rate) || 48000,
                            bitRate: s.bit_rate ? parseInt(s.bit_rate) : 0,
                            language: lang,
                            languageName: langName,
                            title,
                            isDefault: !!(s.disposition && s.disposition.default),
                        });
                    } else if (s.codec_type === 'subtitle') {
                        const codec = codecNames[s.codec_name] || s.codec_name;
                        /* PGS/VobSub are image-based, can't convert to WebVTT */
                        const isTextBased = !['hdmv_pgs_subtitle', 'dvd_subtitle'].includes(s.codec_name);

                        subtitleTracks.push({
                            index: s.index,
                            codec: s.codec_name,
                            codecLabel: codec,
                            language: lang,
                            languageName: langName,
                            title,
                            isDefault: !!(s.disposition && s.disposition.default),
                            isForced: !!(s.disposition && s.disposition.forced),
                            isTextBased,
                        });
                    }
                }

                /*
                 * External subtitle files sitting in the same folder as the
                 * movie (.srt/.ass/.ssa/.vtt/.sub).  Files whose name starts
                 * with the movie's basename sort first.
                 */
                const externalSubs = [];
                try {
                    const dir = path.dirname(file.path);
                    const base = path.basename(file.path, path.extname(file.path)).toLowerCase();
                    const subExts = new Set(['.srt', '.ass', '.ssa', '.vtt', '.sub']);
                    const entries = fs.readdirSync(dir)
                        .filter(n => subExts.has(path.extname(n).toLowerCase()))
                        .sort((a, b) => {
                            const am = a.toLowerCase().startsWith(base) ? 0 : 1;
                            const bm = b.toLowerCase().startsWith(base) ? 0 : 1;
                            return am - bm || a.localeCompare(b);
                        });
                    for (const name of entries) {
                        externalSubs.push({ name });
                    }
                } catch (e) { /* unreadable dir → no external subs */ }

                res.json({ duration, audioTracks, subtitleTracks, externalSubs });
            } catch (e) {
                res.status(500).json({ error: 'Failed to parse tracks' });
            }
        });
    });

    /* Extract subtitles as WebVTT */
    /*
     * Serve an EXTERNAL subtitle file (same folder as the movie) as WebVTT.
     * The name is validated against a fresh directory listing — never joined
     * blindly — so path traversal is impossible.
     */
    app.get('/api/files/:id/extsub', (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).send('File not found');

        const wanted = String(req.query.name || '');
        const dir = path.dirname(file.path);
        const subExts = new Set(['.srt', '.ass', '.ssa', '.vtt', '.sub']);
        let match = null;
        try {
            match = fs.readdirSync(dir).find(n =>
                n === wanted && subExts.has(path.extname(n).toLowerCase()));
        } catch (e) { /* fallthrough */ }
        if (!match) return res.status(404).send('Subtitle not found');
        const extOffset = Math.max(0, parseFloat(req.query.offset) || 0);

        res.set('Content-Type', 'text/vtt; charset=utf-8');
        res.set('Access-Control-Allow-Origin', '*');

        const ffmpeg = spawn(config.FFMPEG_PATH, [
            ...(extOffset > 0 ? ['-ss', String(extOffset)] : []),
            '-i', path.join(dir, match),
            '-f', 'webvtt',
            '-'
        ], { stdio: ['ignore', 'pipe', 'pipe'] });

        ffmpeg.stdout.pipe(res);
        ffmpeg.stderr.on('data', () => {});
        ffmpeg.on('error', () => {
            if (!res.headersSent) res.status(500).send('Subtitle conversion failed');
        });
        req.on('close', () => { try { ffmpeg.kill('SIGTERM'); } catch (e) {} });
    });

    /*
     * TV-native mode: HLS sessions with server-side binaural rendering.
     * GET /api/files/:id/hls?audioTrack=N&channels=C&sofa=name&room=R&t=T
     *   → { url, base } — url is the playlist, base the keyframe-aligned
     *     session start in movie time (the client maps its UI timeline).
     */
    /*
     * Bandwidth probe: stream incompressible bytes until the client aborts
     * (100 MB safety cap). The TV measures its own download rate and reports
     * it as `bw` when starting HLS sessions, so the server can pick the
     * copy-vs-reencode threshold and the NVENC bitrate from the real link.
     */
    const speedBuf = crypto.randomBytes(256 * 1024);
    app.get('/api/speedtest', (req, res) => {
        res.set('Content-Type', 'application/octet-stream');
        res.set('Cache-Control', 'no-store');
        const MAX = 100 * 1024 * 1024;
        let sent = 0, done = false;
        const write = () => {
            if (done) return;
            let ok = true;
            while (ok && sent < MAX) { ok = res.write(speedBuf); sent += speedBuf.length; }
            if (sent >= MAX) { done = true; res.end(); }
        };
        res.on('drain', write);
        req.on('close', () => { done = true; });
        write();
    });

    const hls = createHlsManager(options);

    app.get('/api/files/:id/hls', async (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).json({ error: 'File not found' });

        const audioStreamIndex = parseInt(req.query.audioTrack);
        const channels = Math.min(16, Math.max(1, parseInt(req.query.channels) || 6));
        const room = parseInt(req.query.room);
        let sofaPath = null;
        if (req.query.sofa) {
            sofaPath = listHrtfProfiles().get(String(req.query.sofa)) || null;
        }

        try {
            const session = await hls.startSession(file, {
                audioStreamIndex: Number.isFinite(audioStreamIndex) ? audioStreamIndex : 1,
                channels,
                sofaPath,
                room: Number.isFinite(room) ? room : 1,
                t: req.query.t,
                bw: req.query.bw,
            });
            res.json({ url: `/hls/${session.id}/out.m3u8`, base: session.base });
        } catch (e) {
            res.status(500).json({ error: 'HLS session failed: ' + e.message });
        }
    });

    app.get('/api/hls/stop', (req, res) => {
        hls.stopAll();
        res.json({ ok: true });
    });

    /* Serve session playlists/segments from the temp dir. */
    app.get('/hls/:sid/:name', (req, res) => {
        const sid = req.params.sid, name = req.params.name;
        if (!/^[a-z0-9-]+$/.test(sid) || !/^[\w.-]+$/.test(name)) {
            return res.status(400).send('bad path');
        }
        const p = path.join(os.tmpdir(), 'halosound-hls', sid, name);
        if (!fs.existsSync(p)) return res.status(404).send('not found');
        res.set('Access-Control-Allow-Origin', '*');
        res.set('Cache-Control', 'no-cache');
        if (name.endsWith('.m3u8')) res.set('Content-Type', 'application/vnd.apple.mpegurl');
        else if (name.endsWith('.mp4') || name.endsWith('.m4s')) res.set('Content-Type', 'video/mp4');
        res.sendFile(p);
    });

    /*
     * PGS (image-based) subtitle stream as a raw .sup file, for client-side
     * rendering with libpgs.  Extraction has to demux the whole container,
     * so results are cached in the OS temp dir (keyed by file id + stream +
     * source size, so replacing the movie invalidates).  Concurrent requests
     * for the same track share one extraction.
     */
    const pgsInflight = new Map();

    app.get('/api/files/:id/pgssub/:streamIndex', (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).send('File not found');
        const streamIndex = parseInt(req.params.streamIndex);
        if (!Number.isFinite(streamIndex) || streamIndex < 0) {
            return res.status(400).send('Bad stream index');
        }

        const cacheDir = path.join(os.tmpdir(), 'halosound-pgs');
        try { fs.mkdirSync(cacheDir, { recursive: true }); } catch (e) {}
        let st = null;
        try { st = fs.statSync(file.path); } catch (e) {}
        const key = `${file.id}-${streamIndex}-${st ? st.size : 0}.sup`;
        const cached = path.join(cacheDir, key);

        const sendSup = () => {
            res.set('Content-Type', 'application/octet-stream');
            res.set('Access-Control-Allow-Origin', '*');
            res.sendFile(cached);
        };

        try {
            if (fs.existsSync(cached) && fs.statSync(cached).size > 0) return sendSup();
        } catch (e) { /* fall through to extraction */ }

        let job = pgsInflight.get(key);
        if (!job) {
            job = new Promise((resolve, reject) => {
                const tmp = cached + '.part';
                const ffmpeg = spawn(config.FFMPEG_PATH, [
                    '-v', 'error', '-y',
                    '-i', file.path,
                    '-map', `0:${streamIndex}`,
                    '-c:s', 'copy',
                    '-f', 'sup',
                    tmp
                ], { stdio: ['ignore', 'ignore', 'pipe'] });
                let errOut = '';
                ffmpeg.stderr.on('data', d => { errOut += d; });
                ffmpeg.on('close', (code) => {
                    if (code === 0) {
                        try { fs.renameSync(tmp, cached); resolve(); }
                        catch (e) { reject(e); }
                    } else {
                        try { fs.unlinkSync(tmp); } catch (e) {}
                        reject(new Error(errOut.slice(0, 200) || `ffmpeg exit ${code}`));
                    }
                });
                ffmpeg.on('error', reject);
            }).finally(() => pgsInflight.delete(key));
            pgsInflight.set(key, job);
            if (options.verbose) console.log(`[PGS] extracting stream ${streamIndex} of ${file.name}`);
        }

        job.then(sendSup).catch((e) => {
            if (!res.headersSent) res.status(500).send('PGS extraction failed: ' + e.message);
        });
    });

    /*
     * HRTF profiles: list and serve .sofa files so the TV app can offer
     * more than its bundled default.  Profiles live in the media folder
     * (root or an hrtf/ subfolder).
     */
    function hrtfDirs() {
        const dirs = [];
        if (options.mediaDir) {
            dirs.push(options.mediaDir);
            dirs.push(path.join(options.mediaDir, 'hrtf'));
        }
        return dirs;
    }

    function listHrtfProfiles() {
        const seen = new Map();
        for (const dir of hrtfDirs()) {
            try {
                for (const n of fs.readdirSync(dir)) {
                    if (path.extname(n).toLowerCase() === '.sofa' && !seen.has(n)) {
                        seen.set(n, path.join(dir, n));
                    }
                }
            } catch (e) { /* dir missing → skip */ }
        }
        return seen;
    }

    app.get('/api/hrtf', (req, res) => {
        const names = [...listHrtfProfiles().keys()];
        res.json(names.map(n => ({ name: n })));
    });

    app.get('/api/hrtf/:name', (req, res) => {
        const profiles = listHrtfProfiles();
        const p = profiles.get(req.params.name);   // exact match only
        if (!p) return res.status(404).send('Profile not found');
        res.set('Content-Type', 'application/octet-stream');
        res.sendFile(p);
    });

    app.get('/api/files/:id/subtitles/:streamIndex', (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).send('File not found');

        const streamIndex = parseInt(req.params.streamIndex);
        /* offset: shift subtitle times so they match an HLS session whose
         * timeline starts at `offset` seconds into the movie. */
        const offset = Math.max(0, parseFloat(req.query.offset) || 0);

        res.set('Content-Type', 'text/vtt; charset=utf-8');
        res.set('Access-Control-Allow-Origin', '*');

        const ffmpeg = spawn(config.FFMPEG_PATH, [
            ...(offset > 0 ? ['-ss', String(offset)] : []),
            '-i', file.path,
            '-map', `0:${streamIndex}`,
            '-f', 'webvtt',
            '-'
        ], { stdio: ['ignore', 'pipe', 'pipe'] });

        ffmpeg.stdout.pipe(res);

        ffmpeg.stderr.on('data', () => {});
        ffmpeg.on('error', () => {
            if (!res.headersSent) res.status(500).send('Subtitle extraction failed');
        });

        req.on('close', () => {
            try { ffmpeg.kill('SIGTERM'); } catch (e) {}
        });
    });

    /* Serve video file with HTTP range request support */
    app.get('/video/:id', (req, res) => {
        const file = files.find(f => f.id === parseInt(req.params.id));
        if (!file) return res.status(404).send('File not found');

        const stat = fs.statSync(file.path);
        const fileSize = stat.size;
        const range = req.headers.range;

        /* Map extension to MIME type */
        const mimeTypes = {
            '.mkv': 'video/x-matroska',
            '.mp4': 'video/mp4',
            '.avi': 'video/x-msvideo',
            '.m4v': 'video/mp4',
            '.ts': 'video/mp2t',
            '.webm': 'video/webm'
        };
        const contentType = mimeTypes[file.ext] || 'application/octet-stream';

        if (range) {
            /* Parse Range header: bytes=start-end */
            const parts = range.replace(/bytes=/, '').split('-');
            const start = parseInt(parts[0], 10);
            const end = parts[1] ? parseInt(parts[1], 10) : fileSize - 1;
            const chunksize = (end - start) + 1;

            if (options.verbose) {
                console.log(`  Range: ${start}-${end}/${fileSize}`);
            }

            const stream = fs.createReadStream(file.path, { start, end });
            res.writeHead(206, {
                'Content-Range': `bytes ${start}-${end}/${fileSize}`,
                'Accept-Ranges': 'bytes',
                'Content-Length': chunksize,
                'Content-Type': contentType
            });
            stream.pipe(res);
        } else {
            /* Full file */
            res.writeHead(200, {
                'Content-Length': fileSize,
                'Content-Type': contentType,
                'Accept-Ranges': 'bytes'
            });
            fs.createReadStream(file.path).pipe(res);
        }
    });

    return app;
}

module.exports = { createHttpServer };
