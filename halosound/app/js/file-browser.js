// File browser — renders the media list. D-pad navigation and OK activation
// are handled generically by the FocusEngine (items are `.focusable`).
class FileBrowser {
    constructor(connection) {
        this.connection = connection;
        this.files = [];
        this.listElement = document.getElementById('file-list');
        this.onFileSelected = null;
    }

    async loadFiles() {
        this.files = await this.connection.getFiles();
        this.render();
    }

    render() {
        this.listElement.innerHTML = '';
        if (!this.files.length) {
            const empty = document.createElement('div');
            empty.className = 'file-empty';
            empty.textContent = 'No media found in the server folder.';
            this.listElement.appendChild(empty);
            return;
        }
        this.files.forEach((file, i) => {
            const item = document.createElement('div');
            item.className = 'file-item focusable';
            item.dataset.index = i;

            const name = document.createElement('span');
            name.className = 'file-name';
            name.textContent = file.name;

            const size = document.createElement('span');
            size.className = 'file-size';
            size.textContent = this.formatSize(file.size);

            item.appendChild(name);
            item.appendChild(size);
            item.addEventListener('click', () => this.selectAndPlay(i));
            this.listElement.appendChild(item);
        });
    }

    selectAndPlay(index) {
        const file = this.files[index];
        if (file && this.onFileSelected) this.onFileSelected(file);
    }

    formatSize(bytes) {
        if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + ' GB';
        if (bytes >= 1048576) return (bytes / 1048576).toFixed(0) + ' MB';
        return (bytes / 1024).toFixed(0) + ' KB';
    }
}
