# Headphone EQ Profiles

Drop AutoEQ-style `ParametricEq.txt` files here and they'll appear in the
**Headphone EQ** dropdown inside mpv-sofa's Control Panel.

## What this does

Most headphones are not flat — they have ±5–10 dB of frequency colouration
relative to a neutral target (Harman, diffuse-field, etc.). When you listen
to HRTF-rendered binaural audio through them, that colouration **rides on
top** of the carefully-computed spatial cues, distorting the imaging.

A headphone EQ profile inverts the headphone's measured response so the
sound that reaches your ear matches the intended target curve. Combined
with HRTF, you finally hear what the renderer is actually producing.

## Where to get profiles

**AutoEQ** — https://github.com/jaakkopasanen/AutoEq
The largest open database of headphone measurements with ready-to-use
parametric EQ corrections. Over 5000 headphone models.

How to get yours:
1. Open https://github.com/jaakkopasanen/AutoEq/tree/master/results
2. Browse to your headphone (e.g., `oratory1990/sennheiser hd 600`).
3. Download `ParametricEq.txt`.
4. Rename to something descriptive (e.g., `Sennheiser HD 600.txt`).
5. Drop it in this folder.
6. Restart mpv-sofa or reopen the Control Panel.

## Format

The expected format matches AutoEQ's `ParametricEq.txt`:

```
Preamp: -6.0 dB
Filter 1: ON PK Fc 50 Hz Gain -3.5 dB Q 1.4
Filter 2: ON PK Fc 200 Hz Gain 1.5 dB Q 0.8
Filter 3: ON LSC Fc 105 Hz Gain 4.5 dB Q 0.7
Filter 4: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.7
```

Recognised filter types: `PK` (peaking), `LS`/`LSC` (low shelf), `HS`/`HSC`
(high shelf). Up to 16 bands per profile. The `Preamp:` line is honoured —
AutoEQ uses it to absorb peaks before clipping.

Filters marked `OFF` are skipped silently.

## Tip

Pair the AutoEQ correction with the **Harman target** (their default
exporting). The HRTF was calibrated assuming a ~neutral target, so anything
close to Harman/diffuse will give the most accurate rendering.

If a profile makes content sound wrong (too thin, too dark), you may have
the wrong measurement — different copies of the same headphone model can
vary, and pad/seal differences shift the response. Audio companies disagree
on "neutral"; experiment.
