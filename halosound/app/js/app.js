// mpv-sofa client — main entry point.
(function() {
    'use strict';

    const focus = new FocusEngine();
    const connection = new HaloConnection();
    const audioEngine = new HaloAudioEngine();
    const ui = new UIController(focus);
    const menu = new MainMenu(focus);

    let player = null;
    let browser = null;
    let settings = null;
    let visualizer = null;
    let trackSelector = null;
    let pendingFile = null;      // file waiting for track selection
    let overlayTimer = null;

    // ---- Screen / focus management ---------------------------------------

    function screenEl(name) { return document.getElementById('screen-' + name); }

    function defaultFocus(name) {
        switch (name) {
            case 'connect':  return savedIp() ? '#btn-connect' : '#server-ip';
            case 'browser':  return '.file-item';
            case 'tracks':   return '#btn-play-tracks';
            case 'settings': return '.toggle-btn';
            default:         return null;
        }
    }

    function showScreen(name) {
        ui.currentScreen = name;
        document.querySelectorAll('.screen').forEach(s => s.classList.remove('active'));
        const el = screenEl(name);
        if (el) el.classList.add('active');

        if (name === 'player') {
            // Player owns navigation: arrows seek, OK toggles play/pause,
            // any key wakes the overlay.  All defer to the menu when open.
            ui.onNav = (dir) => {
                if (menu.isOpen() || !player) return false;
                if (dir === 'left')  player.seekRelative(-10);
                if (dir === 'right') player.seekRelative(10);
                wakeOverlay();
                return true;
            };
            ui.onOk = () => {
                if (menu.isOpen() || !player) return false;
                player.togglePlayPause();
                wakeOverlay();
                return true;
            };
            ui.onAnyKey = () => { if (!menu.isOpen()) wakeOverlay(); };
            focus.setScope(el);         // no focusables → engine idle here
        } else {
            ui.onNav = ui.onOk = ui.onAnyKey = null;
            focus.setScope(el, defaultFocus(name));
        }
    }

    // ---- Back / main menu -------------------------------------------------

    function handleBack() {
        if (focus.editing) { focus.exitEdit(); return; }
        if (menu.isOpen()) { menu.hide(); return; }
        switch (ui.currentScreen) {
            case 'connect':  exitApp(); break;
            case 'tracks':   showScreen('browser'); refreshFileList(); break;
            case 'settings': showScreen('browser'); break;
            case 'browser':
            case 'player':   openMenu(); break;
            default:         openMenu(); break;
        }
    }

    function openMenu() {
        // Freeze playback while the menu sits over the video.
        if (ui.currentScreen === 'player' && player && player.playing) {
            player.pause();
        }
        const server = connection.httpBase
            ? connection.httpBase.replace(/^https?:\/\//, '') : '';
        menu.show({ playing: !!(player && player.currentFile), server });
    }

    menu.onAction = (action) => {
        menu.hide();
        switch (action) {
            case 'resume':
                showScreen('player');
                if (player) player.resume();
                break;
            case 'library':
                showScreen('browser');
                refreshFileList();
                break;
            case 'settings':
                showScreen('settings');
                break;
            case 'server':
                if (player) player.stop();
                if (visualizer) visualizer.stop();
                connection.disconnect();
                showScreen('connect');
                break;
            case 'exit':
                exitApp();
                break;
        }
    };

    function exitApp() {
        if (window.webOS && typeof webOS.platformBack === 'function') {
            webOS.platformBack();
        } else {
            window.close();
        }
    }

    // ---- Player overlay auto-hide ----------------------------------------

    function wakeOverlay() {
        const overlay = document.getElementById('player-overlay');
        if (!overlay) return;
        overlay.classList.add('visible');
        clearTimeout(overlayTimer);
        overlayTimer = setTimeout(() => overlay.classList.remove('visible'), 4000);
    }

    // ---- Connection persistence ------------------------------------------

    function savedIp()   { try { return localStorage.getItem('mpvsofa.serverIp'); }   catch (e) { return null; } }
    function savedPort() { try { return localStorage.getItem('mpvsofa.serverPort'); } catch (e) { return null; } }

    function detectServerOrigin() {
        const loc = window.location;
        if (loc.pathname.startsWith('/app')) {
            return {
                ip: loc.hostname,
                httpPort: parseInt(loc.port) || 8080,
                wsPort: (parseInt(loc.port) || 8080) + 1
            };
        }
        return null;
    }

    // ---- Init -------------------------------------------------------------

    async function init() {
        visualizer = new HaloVisualizer('visualizer');

        ui.onBack = handleBack;
        ui.onPlayPause = () => { if (ui.currentScreen === 'player' && player) player.togglePlayPause(); };

        // Header / back buttons still work as direct clicks.
        document.getElementById('btn-connect').addEventListener('click', doConnect);
        document.getElementById('btn-settings').addEventListener('click', () => showScreen('settings'));
        document.getElementById('btn-back-settings').addEventListener('click', () => showScreen('browser'));
        document.getElementById('btn-back-tracks').addEventListener('click', () => {
            showScreen('browser'); refreshFileList();
        });

        // Restore last successful connection.
        const origin = detectServerOrigin();
        if (origin) {
            document.getElementById('server-ip').value = origin.ip;
            document.getElementById('server-port').value = origin.httpPort;
            document.getElementById('connection-status').textContent = 'Auto-detected server, connecting...';
            showScreen('connect');
            setTimeout(() => doConnect(), 300);
            return;
        }
        const ip = savedIp(), port = savedPort();
        if (ip)   document.getElementById('server-ip').value = ip;
        if (port) document.getElementById('server-port').value = port;
        showScreen('connect');
    }

    async function doConnect() {
        const ip = document.getElementById('server-ip').value.trim();
        const statusEl = document.getElementById('connection-status');

        const origin = detectServerOrigin();
        const fieldPort = parseInt(document.getElementById('server-port').value, 10);
        const httpPort = origin ? origin.httpPort
                       : (fieldPort >= 1 && fieldPort <= 65535 ? fieldPort : 8080);
        const wsPort = origin ? origin.wsPort : httpPort + 1;

        statusEl.textContent = 'Connecting to ' + ip + ':' + httpPort + '...';

        const ok = await connection.connect(ip, httpPort, wsPort);
        if (!ok) {
            statusEl.textContent = 'Connection failed. Check IP and try again.';
            return;
        }
        statusEl.textContent = 'Connected!';
        try {
            localStorage.setItem('mpvsofa.serverIp', ip);
            localStorage.setItem('mpvsofa.serverPort', String(httpPort));
        } catch (e) { /* localStorage unavailable */ }

        await audioEngine.init();
        try { await audioEngine.loadWasm(); console.log('WASM DSP loaded'); }
        catch (e) { console.warn('WASM load failed, using fallback:', e); }
        try { await audioEngine.loadSofa('assets/hrtf/default.sofa'); console.log('Default SOFA loaded'); }
        catch (e) { console.warn('SOFA load failed:', e); }

        player = new HaloPlayer(connection, audioEngine);
        browser = new FileBrowser(connection);
        settings = new HaloSettings(audioEngine);
        trackSelector = new TrackSelector(connection);

        player.onPlaybackEnded = () => { if (visualizer) visualizer.stop(); showScreen('browser'); refreshFileList(); };
        player.onAudioInfo = (info) => settings.updateAudioInfo(info);

        document.getElementById('mute-bed').addEventListener('change', (e) =>
            audioEngine.setChannelMute('bed', e.target.checked));
        document.getElementById('mute-height').addEventListener('change', (e) =>
            audioEngine.setChannelMute('height', e.target.checked));

        browser.onFileSelected = (file) => showTrackSelector(file);
        trackSelector.onPlay = (selection) => startPlayback(pendingFile, selection);

        await browser.loadFiles();
        showScreen('browser');
    }

    async function showTrackSelector(file) {
        pendingFile = file;
        showScreen('tracks');
        await trackSelector.loadTracks(file.id);
        focus.refresh('#btn-play-tracks');   // re-scan now that tracks exist
    }

    async function startPlayback(file, selection) {
        document.getElementById('player-title').textContent = file.name;
        showScreen('player');
        wakeOverlay();
        if (visualizer) visualizer.start();
        await player.play(file, selection);
        document.getElementById('hrtf-status').textContent =
            'HRTF: ' + (audioEngine.wasmReady ? 'Active' : 'Fallback');
    }

    function refreshFileList() {
        if (browser) browser.loadFiles().then(() => focus.refresh('.file-item')).catch(() => {});
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
