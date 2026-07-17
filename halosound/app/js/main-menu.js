// main-menu.js — the Back-button main menu overlay.
//
// Instead of exiting the app, Back opens this menu.  It is the central hub:
// Resume (when something is playing), Library, Audio & HRTF settings, Change
// server, and an explicit Exit.  Items are built per-open from context so
// Resume only shows when relevant.  While open it owns a focus scope so the
// underlying screen is inert.
class MainMenu {
    constructor(focus) {
        this.focus = focus;
        this.el = document.getElementById('main-menu');
        this.listEl = document.getElementById('main-menu-list');
        this.footEl = document.getElementById('main-menu-footer');
        this.open = false;
        this.onAction = null;   // (actionId) => void
    }

    isOpen() { return this.open; }

    show(ctx) {
        this._buildItems(ctx || {});
        this._renderFooter(ctx || {});
        this.el.classList.add('visible');
        this.open = true;
        this.focus.pushScope(this.el, '.menu-item');
    }

    hide() {
        if (!this.open) return;
        this.el.classList.remove('visible');
        this.open = false;
        this.focus.popScope();
    }

    _buildItems(ctx) {
        const items = [
            ctx.playing ? { id: 'resume',   label: 'Resume playback', icon: '▶' } : null,
            { id: 'library',  label: 'Library',          icon: '▤' },
            { id: 'settings', label: 'Audio & HRTF',     icon: '♪' },
            { id: 'server',   label: 'Change server',    icon: '⇄' },
            { id: 'exit',     label: 'Exit app',         icon: '⏻' },
        ].filter(Boolean);

        this.listEl.innerHTML = '';
        for (const it of items) {
            const b = document.createElement('div');
            b.className = 'menu-item focusable';
            b.dataset.action = it.id;
            b.innerHTML = `<span class="menu-icon">${it.icon}</span>` +
                          `<span class="menu-label">${it.label}</span>`;
            b.addEventListener('click', () => {
                if (this.onAction) this.onAction(it.id);
            });
            this.listEl.appendChild(b);
        }
    }

    _renderFooter(ctx) {
        if (!this.footEl) return;
        const server = ctx.server ? `Connected to ${ctx.server}` : 'Not connected';
        this.footEl.textContent = `mpv-sofa client · ${server}`;
    }
}
