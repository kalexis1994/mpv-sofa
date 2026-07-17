const express = require('express');
const fs = require('fs');
const path = require('path');
const { execFile, spawn } = require('child_process');
const config = require('./config');

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

        res.set('Content-Type', 'text/vtt; charset=utf-8');
        res.set('Access-Control-Allow-Origin', '*');

        const ffmpeg = spawn(config.FFMPEG_PATH, [
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

        res.set('Content-Type', 'text/vtt; charset=utf-8');
        res.set('Access-Control-Allow-Origin', '*');

        const ffmpeg = spawn(config.FFMPEG_PATH, [
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
