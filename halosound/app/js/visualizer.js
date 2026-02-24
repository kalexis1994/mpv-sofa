// HaloSound Channel Level Visualizer
class HaloVisualizer {
    constructor(canvasId) {
        this.canvas = document.getElementById(canvasId);
        this.ctx = this.canvas ? this.canvas.getContext('2d') : null;
        this.levels = new Float32Array(16);
        this.animFrame = null;
    }

    start() {
        if (!this.ctx) return;
        this.draw();
    }

    stop() {
        if (this.animFrame) {
            cancelAnimationFrame(this.animFrame);
            this.animFrame = null;
        }
    }

    updateLevels(channelLevels) {
        for (let i = 0; i < channelLevels.length && i < 16; i++) {
            // Smooth decay
            this.levels[i] = Math.max(channelLevels[i], this.levels[i] * 0.9);
        }
    }

    draw() {
        const ctx = this.ctx;
        const w = this.canvas.width;
        const h = this.canvas.height;

        ctx.clearRect(0, 0, w, h);

        // Background
        ctx.fillStyle = 'rgba(0, 0, 0, 0.5)';
        ctx.beginPath();
        ctx.roundRect(0, 0, w, h, 8);
        ctx.fill();

        // Channel labels
        const labels = ['FL', 'FR', 'FC', 'LFE', 'BL', 'BR', 'SL', 'SR',
                       'TFL', 'TFR', 'TBL', 'TBR'];

        const numCh = Math.min(12, this.levels.length);
        const barWidth = (w - 20) / numCh - 4;
        const maxBarHeight = h - 40;

        for (let i = 0; i < numCh; i++) {
            const x = 10 + i * (barWidth + 4);
            const level = Math.min(1, this.levels[i]);
            const barHeight = level * maxBarHeight;

            // Bar gradient
            const gradient = ctx.createLinearGradient(x, h - 20, x, h - 20 - maxBarHeight);
            gradient.addColorStop(0, '#00ff88');
            gradient.addColorStop(0.7, '#ffcc00');
            gradient.addColorStop(1, '#ff4444');

            ctx.fillStyle = gradient;
            ctx.fillRect(x, h - 20 - barHeight, barWidth, barHeight);

            // Label
            ctx.fillStyle = '#aaa';
            ctx.font = '10px monospace';
            ctx.textAlign = 'center';
            ctx.fillText(labels[i] || 'CH' + i, x + barWidth / 2, h - 6);
        }

        this.animFrame = requestAnimationFrame(() => this.draw());
    }
}
