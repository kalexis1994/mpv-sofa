// HaloSound File Browser
class FileBrowser {
    constructor(connection) {
        this.connection = connection;
        this.files = [];
        this.selectedIndex = 0;
        this.listElement = document.getElementById('file-list');
        this.onFileSelected = null;
    }

    async loadFiles() {
        this.files = await this.connection.getFiles();
        this.render();
    }

    render() {
        this.listElement.innerHTML = '';
        this.files.forEach((file, i) => {
            const item = document.createElement('div');
            item.className = 'file-item' + (i === this.selectedIndex ? ' focused' : '');
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

    navigate(direction) {
        const newIndex = this.selectedIndex + direction;
        if (newIndex >= 0 && newIndex < this.files.length) {
            this.selectedIndex = newIndex;
            this.render();
            // Scroll into view
            const items = this.listElement.querySelectorAll('.file-item');
            if (items[newIndex]) items[newIndex].scrollIntoView({ block: 'nearest' });
        }
    }

    selectCurrent() {
        if (this.files.length > 0) {
            this.selectAndPlay(this.selectedIndex);
        }
    }

    selectAndPlay(index) {
        this.selectedIndex = index;
        const file = this.files[index];
        if (this.onFileSelected) this.onFileSelected(file);
    }

    formatSize(bytes) {
        if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(1) + ' GB';
        if (bytes >= 1048576) return (bytes / 1048576).toFixed(0) + ' MB';
        return (bytes / 1024).toFixed(0) + ' KB';
    }
}
