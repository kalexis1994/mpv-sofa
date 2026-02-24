"""
Debug: compare channel-by-channel RMS and peak between 16ch extract_objects
and 8ch standard decode to find which channels have wrong gain/shift.

The hypothesis is that BL/BR (ch4/ch5 in output) have incorrect output_shift
applied due to thd_channel_order mapping issues.
"""
import numpy as np
import os

SR = 48000
D = os.path.dirname(os.path.abspath(__file__))

r16 = np.fromfile(os.path.join(D, 'raw_16ch.pcm'), dtype=np.float32).reshape(-1, 16)
r8 = np.fromfile(os.path.join(D, 'raw_8ch.pcm'), dtype=np.float32).reshape(-1, 8)

ml = min(r16.shape[0], r8.shape[0])

BED = ['FL','FR','FC','LFE','BL','BR','SL','SR']

print("=" * 70)
print("Channel-by-channel comparison: 16ch extract_objects vs 8ch standard")
print("=" * 70)
print(f"{'Ch':>3s} {'Name':>4s} | {'RMS 16ch':>10s} {'RMS 8ch':>10s} {'Ratio':>8s} | {'Peak 16':>8s} {'Peak 8':>8s} {'Ratio':>8s} | {'Corr':>6s}")
print("-" * 90)

for i in range(8):
    s16 = r16[:ml, i]
    s8 = r8[:ml, i]
    rms16 = np.sqrt(np.mean(s16**2))
    rms8 = np.sqrt(np.mean(s8**2))
    pk16 = np.max(np.abs(s16))
    pk8 = np.max(np.abs(s8))
    ratio_rms = rms16 / max(rms8, 1e-10)
    ratio_pk = pk16 / max(pk8, 1e-10)

    # Correlation
    if rms16 > 1e-6 and rms8 > 1e-6:
        corr = np.corrcoef(s16, s8)[0, 1]
    else:
        corr = 0.0

    flag = ""
    if abs(ratio_rms - 1.0) > 0.1 or abs(corr) < 0.99:
        flag = " *** MISMATCH"

    print(f"{i:3d} {BED[i]:>4s} | {rms16:10.6f} {rms8:10.6f} {ratio_rms:8.4f} | {pk16:8.4f} {pk8:8.4f} {ratio_pk:8.4f} | {corr:6.4f}{flag}")

print()

# Detailed analysis of problematic channels
print("=" * 70)
print("Detailed analysis of BL (ch4) and BR (ch5)")
print("=" * 70)

for ch, name in [(4, 'BL'), (5, 'BR')]:
    s16 = r16[:ml, ch]
    s8 = r8[:ml, ch]
    diff = s16 - s8

    print(f"\n--- {name} (ch{ch}) ---")
    print(f"  Diff RMS: {np.sqrt(np.mean(diff**2)):.6f}")
    print(f"  Diff Peak: {np.max(np.abs(diff)):.6f}")

    # Check if diff looks like a gain change (multiplicative)
    # If s16 = s8 * gain, then s16/s8 should be constant
    mask = np.abs(s8) > 0.01  # avoid div by zero
    if np.sum(mask) > 100:
        ratios = s16[mask] / s8[mask]
        print(f"  Sample ratio (where |s8|>0.01): mean={np.mean(ratios):.4f} std={np.std(ratios):.4f}")
        print(f"    min={np.min(ratios):.4f} max={np.max(ratios):.4f}")

    # Check for sudden jumps in the ratio
    diffs16 = np.abs(np.diff(s16))
    diffs8 = np.abs(np.diff(s8))

    # Find where 16ch has big jumps but 8ch doesn't
    big16 = diffs16 > 0.3
    small8 = diffs8 < 0.1  # 8ch is smooth here
    artifacts = big16 & small8[:len(big16)]

    art_pos = np.where(artifacts)[0]
    print(f"  Artifact positions (16ch jump>0.3, 8ch<0.1): {len(art_pos)}")

    if len(art_pos) > 0:
        # Show first 10 artifacts with context
        for p in art_pos[:10]:
            print(f"    sample {p}: 16ch: {s16[p]:.4f} -> {s16[p+1]:.4f} (jump={s16[p+1]-s16[p]:.4f})")
            print(f"              8ch:  {s8[p]:.4f} -> {s8[p+1]:.4f} (jump={s8[p+1]-s8[p]:.4f})")
            # Show ratio around artifact
            if abs(s8[p]) > 0.001 and abs(s8[p+1]) > 0.001:
                r_before = s16[p] / s8[p]
                r_after = s16[p+1] / s8[p+1]
                print(f"              ratio: {r_before:.4f} -> {r_after:.4f} (change: {r_after-r_before:.4f})")

        # Check periodicity of artifacts
        if len(art_pos) > 2:
            gaps = np.diff(art_pos)
            # Filter out consecutive samples (same glitch event)
            event_starts = [art_pos[0]]
            for j in range(1, len(art_pos)):
                if art_pos[j] - art_pos[j-1] > 10:
                    event_starts.append(art_pos[j])

            print(f"\n  Glitch EVENTS (separated by >10 samples): {len(event_starts)}")
            if len(event_starts) > 1:
                event_gaps = np.diff(event_starts)
                print(f"    Gap between events: min={event_gaps.min()} max={event_gaps.max()} median={np.median(event_gaps):.0f}")
                print(f"    = {event_gaps.min()/SR*1000:.2f}ms to {event_gaps.max()/SR*1000:.2f}ms (AU size ~ {np.median(event_gaps):.0f} samples)")

# Check if the issue is output_shift related by looking at gain ratios
# between events
print("\n" + "=" * 70)
print("Gain ratio analysis around glitch events")
print("=" * 70)

for ch, name in [(4, 'BL'), (5, 'BR')]:
    s16 = r16[:ml, ch]
    s8 = r8[:ml, ch]

    diffs16 = np.abs(np.diff(s16))
    diffs8 = np.abs(np.diff(s8))

    big16 = diffs16 > 0.3
    small8 = diffs8 < 0.1
    artifacts = big16 & small8[:len(big16)]
    art_pos = np.where(artifacts)[0]

    # Get event starts
    events = [art_pos[0]] if len(art_pos) > 0 else []
    for j in range(1, len(art_pos)):
        if art_pos[j] - art_pos[j-1] > 10:
            events.append(art_pos[j])

    if len(events) < 2:
        continue

    print(f"\n--- {name} gain analysis ---")
    for i, ev in enumerate(events[:15]):
        # Average ratio 10 samples before and after the event
        before_start = max(0, ev - 20)
        before_end = ev - 1
        after_start = ev + 2
        after_end = min(ml-1, ev + 22)

        mask_b = np.abs(s8[before_start:before_end]) > 0.005
        mask_a = np.abs(s8[after_start:after_end]) > 0.005

        if np.sum(mask_b) > 3 and np.sum(mask_a) > 3:
            ratio_before = np.median(s16[before_start:before_end][mask_b] / s8[before_start:before_end][mask_b])
            ratio_after = np.median(s16[after_start:after_end][mask_a] / s8[after_start:after_end][mask_a])
            # What shift change would cause this?
            if abs(ratio_before) > 0.01 and abs(ratio_after) > 0.01:
                shift_change = np.log2(abs(ratio_after / ratio_before))
                print(f"  Event {i} @ sample {ev} ({ev/SR*1000:.1f}ms):")
                print(f"    ratio before={ratio_before:.4f} after={ratio_after:.4f}")
                print(f"    gain change: x{ratio_after/ratio_before:.4f} = {shift_change:.2f} bits")
