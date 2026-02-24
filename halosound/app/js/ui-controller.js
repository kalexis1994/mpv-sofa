// HaloSound UI Controller - D-pad and Remote Control Handler
class UIController {
    constructor() {
        this.currentScreen = 'connect';
        this.overlayTimeout = null;
        this.onNavigate = null;
        this.onSelect = null;
        this.onBack = null;
        this.onPlayPause = null;

        document.addEventListener('keydown', (e) => this.handleKey(e));
    }

    handleKey(e) {
        // webOS remote key codes
        const key = e.keyCode || e.which;

        switch (key) {
            case 38: // Up
                e.preventDefault();
                if (this.onNavigate) this.onNavigate('up');
                break;
            case 40: // Down
                e.preventDefault();
                if (this.onNavigate) this.onNavigate('down');
                break;
            case 37: // Left
                e.preventDefault();
                if (this.onNavigate) this.onNavigate('left');
                break;
            case 39: // Right
                e.preventDefault();
                if (this.onNavigate) this.onNavigate('right');
                break;
            case 13: // Enter/OK
                e.preventDefault();
                if (this.onSelect) this.onSelect();
                break;
            case 461: // webOS Back button
            case 8:   // Backspace (for testing)
            case 27:  // Escape
                e.preventDefault();
                if (this.onBack) this.onBack();
                break;
            case 415: // Play
            case 19:  // Pause
            case 32:  // Space (for testing)
                e.preventDefault();
                if (this.onPlayPause) this.onPlayPause();
                break;
            case 412: // Rewind
                if (this.onNavigate) this.onNavigate('rewind');
                break;
            case 417: // Fast Forward
                if (this.onNavigate) this.onNavigate('fastforward');
                break;
        }

        // Show overlay on any key press during playback
        if (this.currentScreen === 'player') {
            this.showOverlay();
        }
    }

    showScreen(name) {
        document.querySelectorAll('.screen').forEach(s => s.classList.remove('active'));
        const screen = document.getElementById('screen-' + name);
        if (screen) screen.classList.add('active');
        this.currentScreen = name;
    }

    showOverlay() {
        const overlay = document.getElementById('player-overlay');
        if (overlay) {
            overlay.classList.add('visible');
            clearTimeout(this.overlayTimeout);
            this.overlayTimeout = setTimeout(() => {
                overlay.classList.remove('visible');
            }, 5000);
        }
    }

    hideOverlay() {
        const overlay = document.getElementById('player-overlay');
        if (overlay) overlay.classList.remove('visible');
    }
}
