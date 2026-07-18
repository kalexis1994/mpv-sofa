// HaloSound Track Selector
// Shows audio and subtitle tracks, lets user choose before playback

class TrackSelector {
    constructor(connection) {
        this.connection = connection;
        this.audioTracks = [];
        this.subtitleTracks = [];
        this.selectedAudio = -1;    // stream index
        this.selectedSubtitle = -1; // stream index, -1 = none
        this.onPlay = null;         // callback({audioTrack, subtitleTrack})

        this.audioListEl = document.getElementById('track-list-audio');
        this.subsListEl = document.getElementById('track-list-subs');

        document.getElementById('btn-play-tracks').addEventListener('click', () => {
            this.confirmPlay();
        });
    }

    async loadTracks(fileId) {
        this.fileId = fileId;
        this.selectedAudio = -1;
        this.selectedSubtitle = -1;

        const resp = await fetch(`${this.connection.httpBase}/api/files/${fileId}/tracks`);
        const data = await resp.json();

        this.audioTracks = data.audioTracks || [];
        this.subtitleTracks = data.subtitleTracks || [];
        this.externalSubs = data.externalSubs || [];
        this.duration = data.duration || 0;
        this.selectedSubtitleExt = null;
        this.selectedSubtitlePgs = -1;

        this.renderAudio();
        this.renderSubtitles();

        // Auto-select best audio track (first one with most channels)
        if (this.audioTracks.length > 0) {
            const best = this.audioTracks.reduce((a, b) => {
                // Prefer: more channels, then TrueHD > EAC3 > DTS > AC3
                const scoreA = a.channels * 10 + this.codecPriority(a.codec);
                const scoreB = b.channels * 10 + this.codecPriority(b.codec);
                return scoreB > scoreA ? b : a;
            });
            this.selectAudio(best.index);
        }
    }

    codecPriority(codec) {
        const p = { 'truehd': 9, 'dts': 7, 'eac3': 6, 'ac3': 5, 'flac': 4, 'aac': 3 };
        return p[codec] || 1;
    }

    renderAudio() {
        this.audioListEl.innerHTML = '';

        for (const t of this.audioTracks) {
            const el = document.createElement('div');
            el.className = 'track-item focusable';
            el.dataset.index = t.index;

            // Badge class
            const isAtmos = t.title.toLowerCase().includes('atmos');
            const isHD = ['truehd', 'dts'].includes(t.codec) ||
                         t.title.toLowerCase().includes('hd');

            let badges = `<span class="track-badge ${isAtmos ? 'atmos' : isHD ? 'hd' : ''}">${t.codecLabel}</span>`;
            badges += `<span class="track-badge">${t.channels}ch ${t.layout}</span>`;
            if (t.languageName) {
                badges += `<span class="track-badge">${t.languageName}</span>`;
            }

            el.innerHTML = `
                <div class="track-main">${badges}</div>
                ${t.title ? `<div class="track-detail">${t.title}</div>` : ''}
            `;

            el.addEventListener('click', () => this.selectAudio(t.index));
            this.audioListEl.appendChild(el);
        }
    }

    renderSubtitles() {
        this.subsListEl.innerHTML = '';

        // "None" option
        const noneEl = document.createElement('div');
        noneEl.className = 'track-item-none focusable selected';
        noneEl.textContent = 'None';
        noneEl.addEventListener('click', () => this.selectSubtitle(-1));
        this.subsListEl.appendChild(noneEl);

        for (const t of this.subtitleTracks) {
            const el = document.createElement('div');
            el.className = 'track-item focusable';
            el.dataset.index = t.index;

            let badges = `<span class="track-badge">${t.codecLabel}</span>`;
            if (t.languageName) {
                badges += `<span class="track-badge">${t.languageName}</span>`;
            }
            if (t.isForced) {
                badges += `<span class="track-badge">Forced</span>`;
            }
            if (!t.isTextBased) {
                badges += `<span class="track-badge">Image</span>`;
            }

            el.innerHTML = `
                <div class="track-main">${badges}</div>
                ${t.title ? `<div class="track-detail">${t.title}</div>` : ''}
            `;

            el.addEventListener('click', () => {
                // Image-based (PGS) subs render through libpgs on a canvas
                // overlay; text subs go through the native <track> element.
                if (t.isTextBased) this.selectSubtitle(t.index);
                else this.selectPgsSub(t.index);
            });

            this.subsListEl.appendChild(el);
        }

        // External subtitle files found next to the movie on the server.
        for (const s of this.externalSubs) {
            const el = document.createElement('div');
            el.className = 'track-item focusable';
            el.dataset.ext = s.name;
            el.innerHTML = `
                <div class="track-main"><span class="track-badge">External</span></div>
                <div class="track-detail">${s.name}</div>
            `;
            el.addEventListener('click', () => this.selectExternalSub(s.name));
            this.subsListEl.appendChild(el);
        }
    }

    selectExternalSub(name) {
        this.selectedSubtitle = -1;
        this.selectedSubtitleExt = name;
        this.selectedSubtitlePgs = -1;
        const noneEl = this.subsListEl.querySelector('.track-item-none');
        if (noneEl) noneEl.classList.remove('selected');
        this.subsListEl.querySelectorAll('.track-item').forEach(el => {
            el.classList.toggle('selected', el.dataset.ext === name);
        });
    }

    selectPgsSub(streamIndex) {
        this.selectedSubtitle = -1;
        this.selectedSubtitleExt = null;
        this.selectedSubtitlePgs = streamIndex;
        const noneEl = this.subsListEl.querySelector('.track-item-none');
        if (noneEl) noneEl.classList.remove('selected');
        this.subsListEl.querySelectorAll('.track-item').forEach(el => {
            el.classList.toggle('selected', parseInt(el.dataset.index) === streamIndex);
        });
    }

    selectAudio(streamIndex) {
        this.selectedAudio = streamIndex;
        this.audioListEl.querySelectorAll('.track-item').forEach(el => {
            el.classList.toggle('selected', parseInt(el.dataset.index) === streamIndex);
        });
    }

    selectSubtitle(streamIndex) {
        this.selectedSubtitle = streamIndex;
        this.selectedSubtitleExt = null;

        // Update "None" item
        const noneEl = this.subsListEl.querySelector('.track-item-none');
        if (noneEl) noneEl.classList.toggle('selected', streamIndex === -1);

        // Update track items
        this.subsListEl.querySelectorAll('.track-item').forEach(el => {
            el.classList.toggle('selected', parseInt(el.dataset.index) === streamIndex);
        });
    }

    confirmPlay() {
        if (this.selectedAudio === -1 && this.audioTracks.length > 0) {
            this.selectedAudio = this.audioTracks[0].index;
        }

        const selTrack = this.audioTracks.find(t => t.index === this.selectedAudio);
        if (this.onPlay) {
            this.onPlay({
                audioTrack: this.selectedAudio,
                audioChannels: selTrack ? selTrack.channels : 6,
                duration: this.duration,
                subtitleTrack: this.selectedSubtitle,
                subtitleExt: this.selectedSubtitleExt
                    ? `${this.connection.httpBase}/api/files/${this.fileId}/extsub?name=${encodeURIComponent(this.selectedSubtitleExt)}`
                    : null,
                subtitlePgs: this.selectedSubtitlePgs >= 0 ? this.selectedSubtitlePgs : null,
            });
        }
    }
}
