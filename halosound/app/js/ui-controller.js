// ui-controller.js — webOS remote key router.
//
// Maps remote/D-pad key codes onto the focus engine and a few app-level
// hooks.  Navigation and OK default to the FocusEngine; screens that need
// custom behaviour (the player: arrows seek, OK toggles play/pause) install
// onNav / onOk overrides.  Back always routes to onBack (the app decides:
// exit edit, close menu, cancel a sub-screen, or open the main menu).
const KEYS = {
    UP: 38, DOWN: 40, LEFT: 37, RIGHT: 39,
    OK: 13, ENTER2: 29443,          // 29443 = some webOS OK variants
    BACK: 461, ESC: 27, BKSP: 8,
    PLAY: 415, PAUSE: 19, PLAYPAUSE: 402, STOP: 413,
    FF: 417, RW: 412, SPACE: 32,
};

class UIController {
    constructor(focus) {
        this.focus = focus;
        this.currentScreen = 'connect';

        // App-level hooks (set by app.js).
        this.onBack = null;         // () => void
        this.onPlayPause = null;    // () => void
        this.onNav = null;          // (dir) => bool  (return true if handled)
        this.onOk = null;           // () => bool     (return true if handled)
        this.onAnyKey = null;       // () => void     (e.g. wake the overlay)

        this.overlayTimeout = null;

        document.addEventListener('keydown', (e) => this.handleKey(e));
    }

    handleKey(e) {
        const k = e.keyCode || e.which;

        // --- Edit mode: native control owns the keys; only Back/OK exit. ---
        if (this.focus.editing) {
            if (k === KEYS.BACK || k === KEYS.ESC || k === KEYS.OK || k === KEYS.ENTER2) {
                e.preventDefault();
                this.focus.exitEdit();
            }
            // Everything else (arrows for sliders/selects, typing) falls
            // through to the focused native element.
            return;
        }

        switch (k) {
            case KEYS.UP:    e.preventDefault(); this._nav('up');    break;
            case KEYS.DOWN:  e.preventDefault(); this._nav('down');  break;
            case KEYS.LEFT:  e.preventDefault(); this._nav('left');  break;
            case KEYS.RIGHT: e.preventDefault(); this._nav('right'); break;

            case KEYS.OK:
            case KEYS.ENTER2:
                e.preventDefault(); this._ok(); break;

            case KEYS.BACK:
            case KEYS.ESC:
            case KEYS.BKSP:
                e.preventDefault();
                if (this.onBack) this.onBack();
                break;

            case KEYS.PLAY:
            case KEYS.PAUSE:
            case KEYS.PLAYPAUSE:
            case KEYS.SPACE:
                e.preventDefault();
                if (this.onPlayPause) this.onPlayPause();
                break;

            case KEYS.FF:    e.preventDefault(); this._nav('right'); break;
            case KEYS.RW:    e.preventDefault(); this._nav('left');  break;

            default:
                return; // don't wake overlay on unknown keys
        }

        if (this.onAnyKey) this.onAnyKey();
    }

    _nav(dir) {
        if (this.onNav && this.onNav(dir)) return;
        this.focus.move(dir);
    }

    _ok() {
        if (this.onOk && this.onOk()) return;
        this.focus.activate();
    }
}
