// HaloSound App - Main Entry Point
(function() {
    'use strict';

    const connection = new HaloConnection();
    const audioEngine = new HaloAudioEngine();
    const ui = new UIController();
    let player = null;
    let browser = null;
    let settings = null;
    let visualizer = null;
    let trackSelector = null;
    let pendingFile = null;  // file waiting for track selection

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

    async function init() {
        visualizer = new HaloVisualizer('visualizer');

        // Connection screen
        document.getElementById('btn-connect').addEventListener('click', doConnect);
        document.getElementById('btn-settings').addEventListener('click', () => {
            ui.showScreen('settings');
        });
        document.getElementById('btn-back-settings').addEventListener('click', () => {
            ui.showScreen('browser');
        });
        document.getElementById('btn-back-tracks').addEventListener('click', () => {
            ui.showScreen('browser');
        });
        document.getElementById('server-ip').addEventListener('keydown', (e) => {
            if (e.key === 'Enter') doConnect();
        });

        // Wire up UI controller
        ui.onNavigate = handleNavigate;
        ui.onSelect = handleSelect;
        ui.onBack = handleBack;
        ui.onPlayPause = handlePlayPause;

        ui.showScreen('connect');

        const origin = detectServerOrigin();
        if (origin) {
            document.getElementById('server-ip').value = origin.ip;
            document.getElementById('server-port').textContent = ':' + origin.httpPort;
            document.getElementById('connection-status').textContent = 'Auto-detected server, connecting...';
            setTimeout(() => doConnect(), 300);
        } else {
            document.getElementById('server-ip').focus();
        }
    }

    async function doConnect() {
        const ip = document.getElementById('server-ip').value.trim();
        const statusEl = document.getElementById('connection-status');
        statusEl.textContent = 'Connecting to ' + ip + '...';

        const origin = detectServerOrigin();
        const httpPort = origin ? origin.httpPort : 8080;
        const wsPort = origin ? origin.wsPort : 8081;

        const ok = await connection.connect(ip, httpPort, wsPort);
        if (ok) {
            statusEl.textContent = 'Connected!';

            await audioEngine.init();

            try {
                await audioEngine.loadWasm();
                console.log('WASM DSP loaded successfully');
            } catch (e) {
                console.warn('WASM load failed, using fallback:', e);
            }

            try {
                await audioEngine.loadSofa('assets/hrtf/default.sofa');
                console.log('Default SOFA loaded');
            } catch (e) {
                console.warn('SOFA load failed:', e);
            }

            player = new HaloPlayer(connection, audioEngine);
            browser = new FileBrowser(connection);
            settings = new HaloSettings(audioEngine);
            trackSelector = new TrackSelector(connection);

            player.onPlaybackEnded = () => {
                ui.showScreen('browser');
                visualizer.stop();
            };

            player.onAudioInfo = (info) => {
                settings.updateAudioInfo(info);
            };

            // Debug mute checkboxes
            document.getElementById('mute-bed').addEventListener('change', (e) => {
                audioEngine.setChannelMute('bed', e.target.checked);
            });
            document.getElementById('mute-height').addEventListener('change', (e) => {
                audioEngine.setChannelMute('height', e.target.checked);
            });

            // File selected → show track selector (not play directly)
            browser.onFileSelected = (file) => {
                showTrackSelector(file);
            };

            // Track selector confirmed → start playback
            trackSelector.onPlay = (selection) => {
                startPlayback(pendingFile, selection);
            };

            await browser.loadFiles();
            ui.showScreen('browser');
        } else {
            statusEl.textContent = 'Connection failed. Check IP and try again.';
        }
    }

    async function showTrackSelector(file) {
        pendingFile = file;
        ui.showScreen('tracks');
        await trackSelector.loadTracks(file.id);
    }

    async function startPlayback(file, selection) {
        document.getElementById('player-title').textContent = file.name;
        ui.showScreen('player');
        ui.showOverlay();
        visualizer.start();

        await player.play(file, selection);

        document.getElementById('hrtf-status').textContent =
            'HRTF: ' + (audioEngine.wasmReady ? 'Active' : 'Fallback');
    }

    function handleNavigate(direction) {
        switch (ui.currentScreen) {
            case 'browser':
                if (direction === 'up') browser.navigate(-1);
                if (direction === 'down') browser.navigate(1);
                break;
            case 'player':
                if (direction === 'left' || direction === 'rewind') player.seekRelative(-10);
                if (direction === 'right' || direction === 'fastforward') player.seekRelative(10);
                break;
        }
    }

    function handleSelect() {
        switch (ui.currentScreen) {
            case 'connect':
                doConnect();
                break;
            case 'browser':
                browser.selectCurrent();
                break;
            case 'tracks':
                trackSelector.confirmPlay();
                break;
            case 'player':
                player.togglePlayPause();
                break;
        }
    }

    function handleBack() {
        switch (ui.currentScreen) {
            case 'browser':
                connection.disconnect();
                ui.showScreen('connect');
                break;
            case 'tracks':
                ui.showScreen('browser');
                break;
            case 'player':
                player.stop();
                visualizer.stop();
                ui.showScreen('browser');
                break;
            case 'settings':
                ui.showScreen('browser');
                break;
        }
    }

    function handlePlayPause() {
        if (ui.currentScreen === 'player' && player) {
            player.togglePlayPause();
        }
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
