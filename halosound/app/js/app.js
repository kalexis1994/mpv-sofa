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
    let overlayLevel = 0;        // 0 hidden · 1 timeline · 2 controls
    let pausedByMenu = false;    // playback frozen because the menu is up

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
            ui.onNav = playerNav;
            ui.onOk = playerOk;
            ui.onAnyKey = () => { if (overlayLevel > 0 && !menu.isOpen()) resetOverlayTimer(); };
            focus.setScope(el);         // controls become focusable at level 2
        } else {
            ui.onNav = ui.onOk = ui.onAnyKey = null;
            focus.setScope(el, defaultFocus(name));
        }
    }

    // ---- Player: progressive overlay + transport controls ----------------

    // Level 0 → nothing; 1 → title + timeline; 2 → + control buttons (focusable).
    // Down escalates the level, Up collapses it. In seek mode (level < 2)
    // Left/Right seek and OK toggles play/pause; once the control row is up
    // the FocusEngine drives the buttons directly.
    function setOverlay(level) {
        const prev = overlayLevel;
        overlayLevel = Math.max(0, Math.min(2, level));
        const ov = document.getElementById('player-overlay');
        const ctl = document.getElementById('player-controls');
        if (ov)  ov.classList.toggle('visible', overlayLevel >= 1);
        if (ctl) ctl.classList.toggle('hidden', overlayLevel < 2);
        if (overlayLevel >= 2 && prev < 2) { updatePlayPauseIcon(); focus.refresh('#pc-playpause'); }
        if (overlayLevel < 2 && prev >= 2) focus.refresh();   // release control focus
        resetOverlayTimer();
    }

    function resetOverlayTimer() {
        clearTimeout(overlayTimer);
        if (overlayLevel > 0) overlayTimer = setTimeout(() => setOverlay(0), 5000);
    }

    // Keep the overlay awake at its current level (used on seek / play-pause).
    function bumpOverlay() {
        if (overlayLevel < 1) setOverlay(1);
        else resetOverlayTimer();
    }

    function updatePlayPauseIcon() {
        const b = document.getElementById('pc-playpause');
        if (b && player) b.textContent = player.playing ? '⏸' : '▶';
    }

    function playerNav(dir) {
        if (menu.isOpen() || !player) return false;
        if (dir === 'down') { setOverlay(overlayLevel + 1); return true; }
        if (dir === 'up')   { setOverlay(overlayLevel - 1); return true; }
        if (overlayLevel >= 2) return false;   // controls focused → engine moves
        if (dir === 'left')  { player.seekRelative(-10); bumpOverlay(); }
        if (dir === 'right') { player.seekRelative(10);  bumpOverlay(); }
        return true;
    }

    function playerOk() {
        if (menu.isOpen() || !player) return false;
        if (overlayLevel >= 2) return false;   // engine activates focused button
        player.togglePlayPause();
        updatePlayPauseIcon();
        bumpOverlay();
        return true;
    }

    // ---- Back / main menu -------------------------------------------------

    function handleBack() {
        if (focus.editing) { focus.exitEdit(); return; }
        if (menu.isOpen()) {
            menu.hide();
            // Closing the menu back onto a paused video resumes it.
            if (ui.currentScreen === 'player' && pausedByMenu && player) {
                player.resume(); pausedByMenu = false; updatePlayPauseIcon();
            }
            return;
        }
        switch (ui.currentScreen) {
            case 'connect':  exitApp(); break;
            case 'tracks':   showScreen('browser'); refreshFileList(); break;
            case 'settings': showScreen('browser'); break;
            case 'browser':
                // Inside a subfolder, Back goes up a level; at root it opens
                // the menu.
                if (browser && !browser.atRoot()) browser.up();
                else openMenu();
                break;
            case 'player':   openMenu(); break;
            default:         openMenu(); break;
        }
    }

    function openMenu() {
        if (ui.currentScreen === 'player' && player && player.playing) {
            player.pause(); pausedByMenu = true; updatePlayPauseIcon();
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
                if (player) { player.resume(); pausedByMenu = false; updatePlayPauseIcon(); }
                setOverlay(0);
                break;
            case 'library':
                pausedByMenu = false;
                showScreen('browser');
                refreshFileList();
                break;
            case 'settings':
                pausedByMenu = false;
                showScreen('settings');
                break;
            case 'server':
                if (player) player.stop();
                if (visualizer) visualizer.stop();
                pausedByMenu = false;
                connection.disconnect();
                showScreen('connect');
                break;
            case 'exit':
                exitApp();
                break;
        }
    };

    function exitApp() {
        if (window.webOS && typeof webOS.platformBack === 'function') webOS.platformBack();
        else window.close();
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
        ui.onPlayPause = () => { if (ui.currentScreen === 'player' && player) { player.togglePlayPause(); updatePlayPauseIcon(); bumpOverlay(); } };

        // Header / back buttons.
        document.getElementById('btn-connect').addEventListener('click', doConnect);
        document.getElementById('btn-settings').addEventListener('click', () => showScreen('settings'));
        document.getElementById('btn-back-settings').addEventListener('click', () => showScreen('browser'));
        document.getElementById('btn-back-tracks').addEventListener('click', () => { showScreen('browser'); refreshFileList(); });

        // Player control bar.
        document.getElementById('pc-menu').addEventListener('click', () => openMenu());
        document.getElementById('pc-rew').addEventListener('click', () => { if (player) { player.seekRelative(-10); bumpOverlay(); } });
        document.getElementById('pc-fwd').addEventListener('click', () => { if (player) { player.seekRelative(10); bumpOverlay(); } });
        document.getElementById('pc-playpause').addEventListener('click', () => { if (player) { player.togglePlayPause(); updatePlayPauseIcon(); bumpOverlay(); } });

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

        player = new HaloPlayer(connection, audioEngine);
        browser = new FileBrowser(connection);
        settings = new HaloSettings(audioEngine, player, connection);
        trackSelector = new TrackSelector(connection);

        // SOFA profile: server-provided if one is saved, else built-in.
        try { await settings.initHrtf(); }
        catch (e) {
            console.warn('HRTF init failed, loading built-in:', e);
            try { await audioEngine.loadSofa('assets/hrtf/default.sofa'); } catch (e2) {}
        }

        player.onPlaybackEnded = () => { if (visualizer) visualizer.stop(); showScreen('browser'); refreshFileList(); };
        player.onAudioInfo = (info) => settings.updateAudioInfo(info);
        // Server became unreachable: return to the connect screen with a
        // clear message (a deliberate flow, not a mystery reload).
        player.onServerLost = () => {
            if (player) player.stop();
            connection.disconnect();
            showScreen('connect');
            document.getElementById('connection-status').textContent =
                'Lost contact with the server — check it is running, then reconnect.';
        };
        // HRTF profile / room changes restart the server-side render session
        settings.onEngineParamsChanged = () => { if (player) player.restartHls(); };

        document.getElementById('mute-bed').addEventListener('change', (e) => audioEngine.setChannelMute('bed', e.target.checked));
        document.getElementById('mute-height').addEventListener('change', (e) => audioEngine.setChannelMute('height', e.target.checked));

        browser.onFileSelected = (file) => showTrackSelector(file);
        browser.onDirChanged = () => { if (ui.currentScreen === 'browser') focus.refresh('.file-item'); };
        trackSelector.onPlay = (selection) => startPlayback(pendingFile, selection);

        await browser.loadFiles();
        showScreen('browser');
    }

    async function showTrackSelector(file) {
        pendingFile = file;
        showScreen('tracks');
        await trackSelector.loadTracks(file.id);
        focus.refresh('#btn-play-tracks');
    }

    async function startPlayback(file, selection) {
        document.getElementById('player-title').textContent = file.name;
        showScreen('player');
        setOverlay(1);
        if (visualizer) visualizer.start();
        await player.play(file, selection);
        updatePlayPauseIcon();
    }

    function refreshFileList() {
        if (browser) browser.loadFiles().then(() => focus.refresh('.file-item')).catch(() => {});
    }

    // Debug handle for remote inspection (ares-inspect / CDP).
    window.__dbg = {
        focus, connection, audioEngine, menu,
        get player() { return player; },
        get overlayLevel() { return overlayLevel; },
    };

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
