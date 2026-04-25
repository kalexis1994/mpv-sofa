# Impulse Responses for Convolution Reverb

Drop stereo (or mono) WAV files here and they'll appear in the
**Conv reverb IR** dropdown inside mpv-sofa's Control Panel.

## Format requirements

- `.wav` extension
- **Sample rate = 48 000 Hz** (we do not resample at load time)
- 16-bit PCM, 24-bit PCM, 32-bit PCM, or 32-bit IEEE float
- Mono or stereo (mono is duplicated onto both ears)
- Length up to a few seconds is fine — CPU cost scales linearly with IR duration

The filter logs errors to `stderr` / `hrtf_log.txt` if a file can't be
decoded (unsupported bit depth, wrong sample rate, etc.).

## Where to find free IRs

Creative-Commons / public-domain collections of real-room impulse responses:

- **OpenAIR** (Univ. of York) — https://www.openair.hosted.york.ac.uk/
  Large library of real rooms: cathedrals, studios, concert halls. Mostly
  B-format and stereo. Pick stereo WAVs at 48 kHz or resample with Audacity.
- **EchoThief** — http://www.echothief.com/
  Unusual spaces (tunnels, stairwells, bunkers). Stereo, 48 kHz, 32-bit float.
  Good for dramatic / ambient content.
- **Voxengo free impulses** — https://www.voxengo.com/impulses/
  Compact packs of hall/plate/cathedral IRs.

For a cinema-like sensation, try an IR from a medium auditorium with
RT60 ≈ 0.4–0.8 s (e.g. OpenAIR's "Central Hall" or EchoThief's
"Maes Howe"). Pair with the **Cinema** room preset and set both
ER Wet and Conv reverb wet to taste.

## Quick conversion with Audacity

If an IR is not 48 kHz:

1. Open the WAV in Audacity.
2. `Tracks → Resample…` → `48000`.
3. `File → Export → Export as WAV` → pick 16-bit PCM and save in this folder.

## Tip

The **ER Wet (Ambisonic)** slider gives you the early reflections
(0–80 ms, direction-aware). **Conv reverb wet** gives you the long
tail. Both feed from the same distance-weighted mono send so close
sources stay dry and far sources wash out into the room.
