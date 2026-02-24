#!/usr/bin/env node

/**
 * HaloSound Server - Entry Point
 *
 * Companion server that:
 * 1. Scans a directory for video files
 * 2. Serves video via HTTP with range request support
 * 3. Streams multichannel audio PCM via WebSocket
 *
 * Usage:
 *   halosound-server <media-directory> [--port PORT] [--ws-port WS_PORT] [--verbose]
 */

const path = require('path');
const os = require('os');
const config = require('./config');
const { scanDirectory } = require('./file-scanner');
const { createHttpServer } = require('./http-server');
const { createWsServer } = require('./ws-audio');

/* Parse command line arguments */
const args = process.argv.slice(2);
let mediaDir = null;
let httpPort = config.HTTP_PORT;
let wsPort = config.WS_PORT;
let verbose = false;

for (let i = 0; i < args.length; i++) {
    if (args[i] === '--port' && args[i + 1]) {
        httpPort = parseInt(args[++i]);
    } else if (args[i] === '--ws-port' && args[i + 1]) {
        wsPort = parseInt(args[++i]);
    } else if (args[i] === '--verbose' || args[i] === '-v') {
        verbose = true;
    } else if (args[i] === '--help' || args[i] === '-h') {
        console.log(`
HaloSound Server v1.0.0

Usage: halosound-server <media-directory> [options]

Options:
  --port PORT       HTTP port (default: ${config.HTTP_PORT})
  --ws-port PORT    WebSocket port (default: ${config.WS_PORT})
  --verbose, -v     Verbose logging
  --help, -h        Show this help

Examples:
  halosound-server D:\\Movies
  halosound-server /mnt/usb --port 9090
  halosound-server . --verbose
`);
        process.exit(0);
    } else if (!args[i].startsWith('-')) {
        mediaDir = args[i];
    }
}

if (!mediaDir) {
    console.error('Error: No media directory specified.');
    console.error('Usage: halosound-server <media-directory> [--port PORT] [--verbose]');
    process.exit(1);
}

/* Resolve media directory */
mediaDir = path.resolve(mediaDir);

console.log('');
console.log('  ╔═══════════════════════════════════╗');
console.log('  ║        HaloSound Server 1.0        ║');
console.log('  ║   Spatial Audio for Your TV         ║');
console.log('  ╚═══════════════════════════════════╝');
console.log('');

/* Scan for media files */
console.log(`Scanning: ${mediaDir}`);
const files = scanDirectory(mediaDir);
console.log(`Found ${files.length} video file(s):`);
files.forEach(f => {
    const sizeMB = (f.size / 1048576).toFixed(1);
    console.log(`  [${f.id}] ${f.name} (${sizeMB} MB)`);
});
console.log('');

if (files.length === 0) {
    console.warn('Warning: No video files found in the specified directory.');
    console.warn(`Supported formats: ${config.VIDEO_EXTENSIONS.join(', ')}`);
    console.warn('');
}

/* Start HTTP server */
const app = createHttpServer(files, { verbose });
const httpServer = app.listen(httpPort, '0.0.0.0', () => {
    console.log(`HTTP server listening on port ${httpPort}`);
});

httpServer.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
        console.error(`Error: Port ${httpPort} already in use. Kill the old process or use --port <other>`);
    } else {
        console.error('HTTP server error:', err.message);
    }
    process.exit(1);
});

/* Start WebSocket server */
const wss = createWsServer(wsPort, files, { verbose });

wss.on('error', (err) => {
    if (err.code === 'EADDRINUSE') {
        console.error(`Error: Port ${wsPort} already in use. Kill the old process or use --ws-port <other>`);
    } else {
        console.error('WebSocket server error:', err.message);
    }
    process.exit(1);
});

console.log(`WebSocket server listening on port ${wsPort}`);

/* Print network addresses for the TV to connect to */
console.log('');
console.log('Connect your LG TV to:');
const interfaces = os.networkInterfaces();
for (const [name, addrs] of Object.entries(interfaces)) {
    for (const addr of addrs) {
        if (addr.family === 'IPv4' && !addr.internal) {
            console.log(`  http://${addr.address}:${httpPort}  (${name})`);
        }
    }
}
console.log('');
console.log('Press Ctrl+C to stop the server.');

/* Graceful shutdown */
process.on('SIGINT', () => {
    console.log('\nShutting down...');
    wss.close();
    httpServer.close();
    process.exit(0);
});

process.on('SIGTERM', () => {
    wss.close();
    httpServer.close();
    process.exit(0);
});
