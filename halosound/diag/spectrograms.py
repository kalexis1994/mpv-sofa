import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import os, glob

SR = 48000
D = os.path.dirname(os.path.abspath(__file__))

def load(fn, ch):
    d = np.fromfile(fn, dtype=np.float32)
    s = len(d) // ch
    return d[:s*ch].reshape(s, ch)

def spec(ax, sig, title, vmin=-120, vmax=-20):
    ax.specgram(sig, NFFT=4096, Fs=SR, noverlap=3072, cmap='inferno',
                vmin=vmin, vmax=vmax, scale='dB')
    ax.set_title(title, fontsize=9, fontweight='bold')
    ax.set_ylim(0, 22000)
    ax.set_ylabel('Hz')

BED = ['FL','FR','FC','LFE','BL','BR','SL','SR']
OBJ = [f'Obj{i}' for i in range(8)]

print("Loading...")
r16 = load(os.path.join(D, 'raw_16ch.pcm'), 16)
r8 = load(os.path.join(D, 'raw_8ch.pcm'), 8)
print(f"  16ch: {r16.shape}, 8ch: {r8.shape}")

# --- RMS per channel ---
print("\n--- 16ch RMS ---")
for i in range(16):
    rms = np.sqrt(np.mean(r16[:,i]**2))
    pk = np.max(np.abs(r16[:,i]))
    tag = "BED" if i<8 else "OBJ"
    nm = BED[i] if i<8 else OBJ[i-8]
    print(f"  ch{i:2d} {nm:>4s} [{tag}] rms={rms:.6f} peak={pk:.4f}")

# --- 1: Bed channels 16ch ---
print("\nGenerating spectrograms...")
fig, axes = plt.subplots(2, 4, figsize=(20, 6))
for i in range(8):
    ax = axes[i//4, i%4]
    spec(ax, r16[:,i], f'{BED[i]} (ch{i}) - 16ch extract_obj')
fig.suptitle('Bed Channels (16ch extract_objects)', fontsize=13, fontweight='bold')
fig.tight_layout()
fig.savefig(os.path.join(D, '01_bed_16ch.png'), dpi=120)
plt.close()
print("  01_bed_16ch.png")

# --- 2: Object channels 16ch ---
fig, axes = plt.subplots(2, 4, figsize=(20, 6))
for i in range(8):
    ax = axes[i//4, i%4]
    spec(ax, r16[:,8+i], f'{OBJ[i]} (ch{8+i}) - objects')
fig.suptitle('Object Channels (16ch extract_objects)', fontsize=13, fontweight='bold')
fig.tight_layout()
fig.savefig(os.path.join(D, '02_objects_16ch.png'), dpi=120)
plt.close()
print("  02_objects_16ch.png")

# --- 3: Standard 8ch reference ---
fig, axes = plt.subplots(2, 4, figsize=(20, 6))
for i in range(8):
    ax = axes[i//4, i%4]
    spec(ax, r8[:,i], f'{BED[i]} (ch{i}) - 8ch standard')
fig.suptitle('Standard 8ch Decode (NO extract_objects)', fontsize=13, fontweight='bold')
fig.tight_layout()
fig.savefig(os.path.join(D, '03_standard_8ch.png'), dpi=120)
plt.close()
print("  03_standard_8ch.png")

# --- 4: Bed comparison 16ch vs 8ch ---
ml = min(r16.shape[0], r8.shape[0])
fig, axes = plt.subplots(3, 1, figsize=(16, 9))
bed16_L = r16[:ml,0] + r16[:ml,2]*0.707 + r16[:ml,4]*0.707 + r16[:ml,6]*0.707
bed8_L = r8[:ml,0] + r8[:ml,2]*0.707 + r8[:ml,4]*0.707 + r8[:ml,6]*0.707
spec(axes[0], bed16_L, 'Bed Downmix L - 16ch (extract_objects)')
spec(axes[1], bed8_L, 'Bed Downmix L - 8ch (standard)')
diff = bed16_L - bed8_L
spec(axes[2], diff, f'DIFFERENCE (rms={np.sqrt(np.mean(diff**2)):.6f})')
axes[2].set_xlabel('Time (s)')
fig.tight_layout()
fig.savefig(os.path.join(D, '04_bed_comparison.png'), dpi=120)
plt.close()
print("  04_bed_comparison.png")

# --- 5: Objects sum + waveform ---
obj_sum = np.sum(r16[:,8:], axis=1)
fig, axes = plt.subplots(2, 1, figsize=(16, 6))
spec(axes[0], obj_sum, f'All Objects Summed (rms={np.sqrt(np.mean(obj_sum**2)):.6f})')
t = np.arange(len(obj_sum)) / SR
axes[1].plot(t, obj_sum, linewidth=0.3, color='#FF9800')
axes[1].set_title('Objects Sum Waveform', fontsize=9, fontweight='bold')
axes[1].set_xlabel('Time (s)')
axes[1].set_ylabel('Amplitude')
axes[1].set_ylim(-1, 1)
axes[1].grid(True, alpha=0.3)
fig.tight_layout()
fig.savefig(os.path.join(D, '05_objects_sum.png'), dpi=120)
plt.close()
print("  05_objects_sum.png")

# --- 6: Zoomed waveforms (first 0.5s) for glitch detection ---
z = SR // 2
t_z = np.arange(z) / SR
fig, axes = plt.subplots(4, 1, figsize=(16, 10))
axes[0].plot(t_z, r16[:z,0], lw=0.3, color='#2196F3')
axes[0].set_title('FL (ch0) 16ch', fontsize=9)
axes[1].plot(t_z, r8[:z,0], lw=0.3, color='#4CAF50')
axes[1].set_title('FL (ch0) 8ch standard', fontsize=9)
axes[2].plot(t_z, r16[:z,8], lw=0.3, color='#FF9800')
axes[2].set_title('Obj0 (ch8)', fontsize=9)
axes[3].plot(t_z, r16[:z,9], lw=0.3, color='#F44336')
axes[3].set_title('Obj1 (ch9)', fontsize=9)
for ax in axes:
    ax.set_ylim(-0.5, 0.5)
    ax.grid(True, alpha=0.3)
axes[3].set_xlabel('Time (s)')
fig.suptitle('Zoomed Waveforms (first 0.5s)', fontsize=12, fontweight='bold')
fig.tight_layout()
fig.savefig(os.path.join(D, '06_waveform_zoom.png'), dpi=150)
plt.close()
print("  06_waveform_zoom.png")

# --- 7: Discontinuity check ---
print("\n--- Discontinuity check (jumps > 0.3) ---")
for i in range(16):
    d = np.abs(np.diff(r16[:,i]))
    n = np.sum(d > 0.3)
    mx = np.max(d)
    nm = BED[i] if i<8 else OBJ[i-8]
    if n > 0:
        print(f"  ch{i:2d} {nm:>4s}: {n} jumps, max={mx:.4f} *** ARTIFACTS ***")
    else:
        print(f"  ch{i:2d} {nm:>4s}: clean (max_diff={mx:.4f})")

print("\nDone! Files in:", D)
for f in sorted(glob.glob(os.path.join(D, '*.png'))):
    print(f"  {os.path.basename(f)} ({os.path.getsize(f)//1024} KB)")
