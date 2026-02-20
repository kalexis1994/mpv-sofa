#!/usr/bin/env python3
"""Validate Atmos TrueHD extraction quality on real content.

This script runs ffprobe/ffmpeg against a target media file and reports:
- Detected Atmos TrueHD stream
- Decoder warning/error counters (OAMD, degraded fallbacks, etc.)
- PCM output channel activity (RMS/peak per channel)

It is intended to quickly catch regressions in extract_objects builds.
"""

import argparse
import array
import json
import math
import os
import re
import subprocess
from typing import Dict, List, Optional, Tuple


LOG_PATTERNS = [
    r"spatial object extraction",
    r"OAMD\[",
    r"ANOMALY",
    r"anti-clip adjusted",
    r"spatial substream degraded",
    r"spatial/bed blockpos mismatch",
    r"blockpos mismatch",
    r"FALLBACK",
    r"No restart header present",
    r"parity check failed",
    r"checksum failed",
    r"substream \d+ length mismatch",
    r"Lossless check failed",
    r"output_data\[",
]


def run_cmd(cmd: List[str], env: Dict[str, str]) -> Tuple[int, str]:
    p = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        text=True,
        errors="replace",
    )
    return p.returncode, p.stdout


def probe_streams(ffprobe: str, media: str, env: Dict[str, str]) -> dict:
    code, out = run_cmd(
        [ffprobe, "-v", "error", "-print_format", "json", "-show_streams", "-show_format", media],
        env,
    )
    if code != 0:
        raise RuntimeError(f"ffprobe failed ({code})")
    return json.loads(out)


def pick_atmos_stream(probe: dict, forced_index: Optional[int]) -> int:
    if forced_index is not None:
        return forced_index

    for s in probe.get("streams", []):
        if s.get("codec_type") != "audio":
            continue
        if s.get("codec_name") != "truehd":
            continue
        profile = str(s.get("profile", ""))
        if "Atmos" in profile:
            return int(s["index"])

    raise RuntimeError("No Dolby TrueHD + Atmos stream found")


def segment_points(duration_sec: float) -> List[str]:
    if duration_sec <= 0:
        return ["00:10:00", "01:30:00", "02:40:00"]

    p0 = max(0.0, min(duration_sec - 70.0, duration_sec * 0.10))
    p1 = max(0.0, min(duration_sec - 70.0, duration_sec * 0.50))
    p2 = max(0.0, min(duration_sec - 70.0, duration_sec * 0.85))
    return [sec_to_hms(p0), sec_to_hms(p1), sec_to_hms(p2)]


def sec_to_hms(sec: float) -> str:
    sec_i = int(sec)
    h = sec_i // 3600
    m = (sec_i % 3600) // 60
    s = sec_i % 60
    return f"{h:02d}:{m:02d}:{s:02d}"


def analyze_logs(text: str) -> Dict[str, int]:
    return {pat: len(re.findall(pat, text)) for pat in LOG_PATTERNS}


def decode_pcm_s32le(
    ffmpeg: str, media: str, stream_index: int, ss: str, seconds: int, env: Dict[str, str]
) -> array.array:
    cmd = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-extract_objects",
        "1",
        "-ss",
        ss,
        "-t",
        str(seconds),
        "-i",
        media,
        "-map",
        f"0:{stream_index}",
        "-c:a",
        "pcm_s32le",
        "-f",
        "s32le",
        "-",
    ]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
    if p.returncode != 0:
        raise RuntimeError(f"ffmpeg PCM decode failed at {ss}: {p.stderr.decode('utf-8', 'replace')[:400]}")
    pcm = array.array("i")
    pcm.frombytes(p.stdout)
    return pcm


def channel_stats(pcm: array.array, channels: int = 16) -> Dict[str, object]:
    nframes = len(pcm) // channels
    if nframes <= 0:
        return {"frames": 0}

    sumsq = [0.0] * channels
    absmax = [0] * channels
    for i in range(nframes):
        base = i * channels
        for c in range(channels):
            v = pcm[base + c]
            av = v if v >= 0 else -v
            if av > absmax[c]:
                absmax[c] = av
            sumsq[c] += float(v) * float(v)

    norm = float(2**31 - 1)
    rms_dbfs = []
    peak_dbfs = []
    for c in range(channels):
        rms = math.sqrt(sumsq[c] / nframes)
        rms_dbfs.append(round(20.0 * math.log10((rms / norm) + 1e-20), 2))
        peak_dbfs.append(round(20.0 * math.log10((absmax[c] / norm) + 1e-20), 2))

    return {
        "frames": nframes,
        "rms_dbfs": rms_dbfs,
        "peak_dbfs": peak_dbfs,
        "avg_bed_rms_dbfs": round(sum(rms_dbfs[:8]) / 8.0, 2),
        "avg_obj_rms_dbfs": round(sum(rms_dbfs[8:16]) / 8.0, 2),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("media")
    ap.add_argument("--ffmpeg", default=r"F:\hrtf\ffmpeg-src\ffmpeg_g.exe")
    ap.add_argument("--ffprobe", default=r"F:\hrtf\ffmpeg-src\ffprobe_g.exe")
    ap.add_argument("--stream-index", type=int, default=None)
    ap.add_argument("--sample-seconds", type=int, default=30)
    ap.add_argument("--pcm-seconds", type=int, default=10)
    ap.add_argument("--json-out", default="")
    ap.add_argument("--path-prefix", default=r"C:\msys64\mingw64\bin;F:\hrtf\ffmpeg-build\bin;")
    args = ap.parse_args()

    env = dict(os.environ)
    env["PATH"] = args.path_prefix + env.get("PATH", "")

    probe = probe_streams(args.ffprobe, args.media, env)
    duration = float(probe.get("format", {}).get("duration", 0.0))
    stream_index = pick_atmos_stream(probe, args.stream_index)
    points = segment_points(duration)

    segments = []
    for ss in points:
        cmd = [
            args.ffmpeg,
            "-hide_banner",
            "-loglevel",
            "level+warning",
            "-extract_objects",
            "1",
            "-ss",
            ss,
            "-t",
            str(args.sample_seconds),
            "-i",
            args.media,
            "-map",
            f"0:{stream_index}",
            "-c:a",
            "pcm_s32le",
            "-f",
            "null",
            "-",
        ]
        code, log_text = run_cmd(cmd, env)
        log_counts = analyze_logs(log_text)
        pcm = decode_pcm_s32le(args.ffmpeg, args.media, stream_index, ss, args.pcm_seconds, env)
        pcm_stats = channel_stats(pcm)
        segments.append(
            {
                "ss": ss,
                "log_exit": code,
                "log_counts": log_counts,
                "pcm_stats": pcm_stats,
            }
        )

    report = {
        "media": args.media,
        "duration_sec": duration,
        "stream_index": stream_index,
        "segments": segments,
    }

    out = json.dumps(report, indent=2)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            f.write(out)
    print(out)


if __name__ == "__main__":
    main()
