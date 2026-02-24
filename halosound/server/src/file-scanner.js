const fs = require('fs');
const path = require('path');
const config = require('./config');

/* Directories to skip during recursive scan */
const SKIP_DIRS = new Set([
    'node_modules', '.git', 'build', 'dist', '__pycache__',
    '.claude', '.vscode', '.idea', 'external'
]);

/* Minimum file size to consider (avoids tiny test fixtures) */
const MIN_FILE_SIZE = 1024 * 100; /* 100 KB */

/**
 * Recursively scan a directory for video files.
 * @param {string} dirPath - Directory to scan
 * @returns {Array<{id:number, name:string, path:string, size:number, ext:string}>}
 */
function scanDirectory(dirPath) {
    const results = [];
    let nextId = 1;

    function scan(dir) {
        let entries;
        try {
            entries = fs.readdirSync(dir, { withFileTypes: true });
        } catch (e) {
            console.warn(`Cannot read directory: ${dir} (${e.message})`);
            return;
        }

        for (const entry of entries) {
            const fullPath = path.join(dir, entry.name);

            if (entry.isDirectory()) {
                if (!SKIP_DIRS.has(entry.name)) {
                    scan(fullPath);
                }
            } else if (entry.isFile()) {
                const ext = path.extname(entry.name).toLowerCase();
                /* Match only the exact extension (avoids .d.ts matching .ts) */
                const nameLC = entry.name.toLowerCase();
                const isVideo = config.VIDEO_EXTENSIONS.some(vext =>
                    nameLC.endsWith(vext) && !nameLC.endsWith('.d' + vext)
                );
                if (isVideo) {
                    let stat;
                    try {
                        stat = fs.statSync(fullPath);
                    } catch (e) {
                        continue;
                    }
                    if (stat.size < MIN_FILE_SIZE) continue;
                    results.push({
                        id: nextId++,
                        name: entry.name,
                        path: fullPath,
                        size: stat.size,
                        ext: ext
                    });
                }
            }
        }
    }

    scan(dirPath);
    return results;
}

module.exports = { scanDirectory };
