import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os

SR = 48000
D = os.path.dirname(os.path.abspath(__file__))

r16 = np.fromfile(os.path.join(D, 'raw_16ch.pcm'), dtype=np.float32).reshape(-1, 16)
r8 = np.fromfile(os.path.join(D, 'raw_8ch.pcm'), dtype=np.float32).reshape(-1, 8)

# Zoom BL ch4 - show first 1 second
z = SR  # 1 second
t = np.arange(z) / SR

fig, axes = plt.subplots(3, 1, figsize=(16, 9))

# BL 16ch
axes[0].plot(t, r16[:z, 4], lw=0.4, color='#F44336')
axes[0].set_title('BL (ch4) - 16ch extract_objects - ARTIFACTS HERE', fontsize=11, fontweight='bold')
axes[0].set_ylim(-1.1, 1.1)
axes[0].grid(True, alpha=0.3)

# BL 8ch standard
axes[1].plot(t, r8[:z, 4], lw=0.4, color='#4CAF50')
axes[1].set_title('BL (ch4) - 8ch standard (clean reference)', fontsize=11, fontweight='bold')
axes[1].set_ylim(-1.1, 1.1)
axes[1].grid(True, alpha=0.3)

# Diff
ml = min(z, r8.shape[0])
diff = r16[:ml, 4] - r8[:ml, 4]
axes[2].plot(np.arange(ml)/SR, diff, lw=0.4, color='#9C27B0')
axes[2].set_title(f'DIFFERENCE BL: 16ch - 8ch (rms={np.sqrt(np.mean(diff**2)):.4f})', fontsize=11, fontweight='bold')
axes[2].set_ylim(-1.1, 1.1)
axes[2].grid(True, alpha=0.3)
axes[2].set_xlabel('Time (s)')

fig.tight_layout()
fig.savefig(os.path.join(D, '07_BL_zoom.png'), dpi=150)
plt.close()

# Also find exact positions of glitches
diffs = np.abs(np.diff(r16[:, 4]))
glitch_pos = np.where(diffs > 0.3)[0]
print(f"BL glitch positions ({len(glitch_pos)} total):")
for g in glitch_pos[:20]:
    t_s = g / SR
    print(f"  sample {g} ({t_s:.4f}s): {r16[g,4]:.4f} -> {r16[g+1,4]:.4f} (jump={diffs[g]:.4f})")

# Check if glitches are periodic
if len(glitch_pos) > 2:
    gaps = np.diff(glitch_pos)
    print(f"\nGap between glitches: min={gaps.min()} max={gaps.max()} median={np.median(gaps):.0f} samples")
    print(f"  = {gaps.min()/SR*1000:.1f}ms to {gaps.max()/SR*1000:.1f}ms, median={np.median(gaps)/SR*1000:.1f}ms")
