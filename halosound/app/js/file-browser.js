// File browser — folder-tree navigation. Descends into subfolders and back
// up; D-pad navigation and OK activation come from the FocusEngine (every
// row is `.focusable`).
class FileBrowser {
    constructor(connection) {
        this.connection = connection;
        this.currentDir = '';       // relative path, '' = library root
        this.folders = [];
        this.files = [];
        this.listElement = document.getElementById('file-list');
        this.pathElement = document.getElementById('browser-path');
        this.onFileSelected = null;
        this.onDirChanged = null;   // () => void, so the app can refocus
    }

    atRoot() { return this.currentDir === ''; }

    /* Load/refresh the current directory (used on open and on refresh). */
    async loadFiles() {
        return this.loadDir(this.currentDir);
    }

    async loadDir(dir) {
        const data = await this.connection.browse(dir);
        this.currentDir = data.dir || '';
        this.folders = data.folders || [];
        this.files = data.files || [];
        this.render();
    }

    descend(folder) {
        const next = this.currentDir ? `${this.currentDir}/${folder}` : folder;
        this.loadDir(next).then(() => this.onDirChanged && this.onDirChanged());
    }

    up() {
        if (this.atRoot()) return;
        const parts = this.currentDir.split('/');
        parts.pop();
        this.loadDir(parts.join('/')).then(() => this.onDirChanged && this.onDirChanged());
    }

    render() {
        const el = this.listElement;
        el.innerHTML = '';

        if (this.pathElement) {
            this.pathElement.textContent = this.currentDir ? '/' + this.currentDir : '';
        }

        // "Up" row when inside a subfolder.
        if (!this.atRoot()) {
            const up = document.createElement('div');
            up.className = 'file-item folder-item up-item focusable';
            up.innerHTML = `<span class="file-icon">↰</span><span class="file-name">..</span>`;
            up.addEventListener('click', () => this.up());
            el.appendChild(up);
        }

        // Folders first.
        for (const folder of this.folders) {
            const item = document.createElement('div');
            item.className = 'file-item folder-item focusable';
            item.innerHTML =
                `<span class="file-icon">📁</span>` +
                `<span class="file-name"></span>` +
                `<span class="file-size">›</span>`;
            item.querySelector('.file-name').textContent = folder;
            item.addEventListener('click', () => this.descend(folder));
            el.appendChild(item);
        }

        // Then files.
        this.files.forEach((file) => {
            const item = document.createElement('div');
            item.className = 'file-item focusable';
            item.innerHTML =
                `<span class="file-icon">🎬</span>` +
                `<span class="file-name"></span>` +
                `<span class="file-size"></span>`;
            item.querySelector('.file-name').textContent = file.name;
            item.querySelector('.file-size').textContent = this.formatSize(file.size);
            item.addEventListener('click', () => {
                if (this.onFileSelected) this.onFileSelected(file);
            });
            el.appendChild(item);
        });

        if (!this.folders.length && !this.files.length) {
            const empty = document.createElement('div');
            empty.className = 'file-empty';
            empty.textContent = this.atRoot()
                ? 'No media found in the server folder.'
                : 'This folder is empty.';
            el.appendChild(empty);
        }
    }

    formatSize(bytes) {
        if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + ' GB';
        if (bytes >= 1048576) return (bytes / 1048576).toFixed(0) + ' MB';
        return (bytes / 1024).toFixed(0) + ' KB';
    }
}
