#!/usr/bin/env python3
"""Compare patched ffmpeg extract_objects output against truehdd reference.

Workflow per segment:
1) Extract Atmos TrueHD elementary stream (.thd) from source media
2) Decode .thd with truehdd (presentation 3) -> 7.1.4 reference audio
3) Decode .thd with patched ffmpeg -extract_objects 1 -> 16ch WAV
4) Compare first 12 channels (7.1.4 domain) with per-channel RMS + corr matrix
"""

from __future__ import annotations

import argparse
import array
import json
import math
import os
import re
import subprocess
import wave
from pathlib import Path
from typing import Dict, List, Tuple


def run_cmd(cmd: List[str], env: Dict[str, str]) -> Tuple[int, str]:
    p = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        env=env,
    )
    return p.returncode, p.stdout


def ensure_ok(code: int, out: str, what: str) -> None:
    if code != 0:
        raise RuntimeError(f"{what} failed ({code}):\n{out[:4000]}")


def read_wav_i32(path: Path) -> Tuple[int, int, array.array]:
    with wave.open(str(path), "rb") as w:
        channels = w.getnchannels()
        sw = w.getsampwidth()
        frames = w.getnframes()
        if sw != 4:
            raise RuntimeError(f"{path} is not 32-bit PCM (sampwidth={sw})")
        data = array.array("i")
        data.frombytes(w.readframes(frames))
    return channels, frames, data


def per_ch_rms_dbfs(data: array.array, channels: int, frames: int) -> List[float]:
    norm = float(2**31 - 1)
    out: List[float] = []
    for c in range(channels):
        s = 0.0
        for i in range(c, len(data), channels):
            v = float(data[i])
            s += v * v
        rms = math.sqrt(s / max(frames, 1))
        out.append(round(20.0 * math.log10((rms / norm) + 1e-20), 2))
    return out


def corr_matrix(
    a: array.array,
    b: array.array,
    ch: int,
    frames: int,
    sample_step: int = 8,
) -> List[List[float]]:
    idx = list(range(0, frames, sample_step))

    def series(src: array.array, c: int) -> List[int]:
        return [src[i * ch + c] for i in idx]

    aa = [series(a, c) for c in range(ch)]
    bb = [series(b, c) for c in range(ch)]

    def stats(v: List[int]) -> Tuple[float, float]:
        n = len(v)
        m = sum(v) / max(n, 1)
        vv = sum((x - m) * (x - m) for x in v)
        return m, vv

    ast = [stats(v) for v in aa]
    bst = [stats(v) for v in bb]

    mtx: List[List[float]] = []
    for i in range(ch):
        row: List[float] = []
        am, av = ast[i]
        for j in range(ch):
            bm, bv = bst[j]
            if av <= 0.0 or bv <= 0.0:
                row.append(0.0)
                continue
            num = 0.0
            va = aa[i]
            vb = bb[j]
            for x, y in zip(va, vb):
                num += (x - am) * (y - bm)
            row.append(num / math.sqrt(av * bv))
        mtx.append(row)
    return mtx


def greedy_match_abs(mtx: List[List[float]]) -> List[Dict[str, float]]:
    ch = len(mtx)
    used = set()
    out = []
    for i in range(ch):
        best_j = -1
        best = -1.0
        for j in range(ch):
            if j in used:
                continue
            v = abs(mtx[i][j])
            if v > best:
                best = v
                best_j = j
        used.add(best_j)
        out.append(
            {
                "ffmpeg_ch": i,
                "truehdd_ch": best_j,
                "corr": round(mtx[i][best_j], 6),
            }
        )
    return out


def pick_atmos_stream(ffprobe: str, media: str, env: Dict[str, str]) -> int:
    code, out = run_cmd(
        [ffprobe, "-v", "error", "-show_streams", "-of", "json", media], env
    )
    ensure_ok(code, out, "ffprobe")
    obj = json.loads(out)
    for s in obj.get("streams", []):
        if s.get("codec_type") != "audio":
            continue
        if s.get("codec_name") != "truehd":
            continue
        if "Atmos" in str(s.get("profile", "")):
            return int(s["index"])
    raise RuntimeError("No Atmos TrueHD stream found")


def count_pat(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("media")
    ap.add_argument("--ss", default="01:29:42")
    ap.add_argument("--duration", type=int, default=30)
    ap.add_argument("--stream-index", type=int, default=None)
    ap.add_argument("--ffmpeg", default=r"F:\hrtf\ffmpeg-src\ffmpeg_g.exe")
    ap.add_argument("--ffprobe", default=r"F:\hrtf\ffmpeg-src\ffprobe_g.exe")
    ap.add_argument("--truehdd", default=r"F:\hrtf\truehdd\target\release\truehdd.exe")
    ap.add_argument("--path-prefix", default=r"C:\msys64\mingw64\bin;F:\hrtf\ffmpeg-src;")
    ap.add_argument("--workdir", default=r"F:\hrtf\tmp")
    ap.add_argument("--prefix", default="cmp_lotr2")
    ap.add_argument("--json-out", default="")
    args = ap.parse_args()

    env = dict(os.environ)
    env["PATH"] = args.path_prefix + env.get("PATH", "")

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)

    stream_index = args.stream_index
    if stream_index is None:
        stream_index = pick_atmos_stream(args.ffprobe, args.media, env)

    thd = work / f"{args.prefix}.thd"
    ff16 = work / f"{args.prefix}.ffmpeg16.wav"
    ff12 = work / f"{args.prefix}.ffmpeg12.wav"
    ref_base = work / f"{args.prefix}.ref"
    ref_audio = work / f"{args.prefix}.ref.atmos.audio"
    ref12 = work / f"{args.prefix}.truehdd12.wav"

    code, out = run_cmd(
        [
            args.ffmpeg,
            "-hide_banner",
            "-y",
            "-ss",
            args.ss,
            "-t",
            str(args.duration),
            "-i",
            args.media,
            "-map",
            f"0:{stream_index}",
            "-vn",
            "-sn",
            "-dn",
            "-c",
            "copy",
            "-f",
            "truehd",
            str(thd),
        ],
        env,
    )
    ensure_ok(code, out, "extract .thd")

    code, out = run_cmd(
        [
            args.truehdd,
            "decode",
            "--presentation",
            "3",
            "--output-path",
            str(ref_base),
            str(thd),
        ],
        env,
    )
    ensure_ok(code, out, "truehdd decode")

    code, ff16_log = run_cmd(
        [
            args.ffmpeg,
            "-hide_banner",
            "-y",
            "-extract_objects",
            "1",
            "-i",
            str(thd),
            "-c:a",
            "pcm_s32le",
            str(ff16),
        ],
        env,
    )
    ensure_ok(code, ff16_log, "ffmpeg extract_objects decode")

    code, out = run_cmd(
        [
            args.ffmpeg,
            "-hide_banner",
            "-y",
            "-i",
            str(ff16),
            "-filter_complex",
            "pan=12c|c0=c0|c1=c1|c2=c2|c3=c3|c4=c4|c5=c5|c6=c6|c7=c7|c8=c8|c9=c9|c10=c10|c11=c11",
            "-c:a",
            "pcm_s32le",
            str(ff12),
        ],
        env,
    )
    ensure_ok(code, out, "ffmpeg 16->12 pan")

    code, out = run_cmd(
        [
            args.ffmpeg,
            "-hide_banner",
            "-y",
            "-i",
            str(ref_audio),
            "-c:a",
            "pcm_s32le",
            str(ref12),
        ],
        env,
    )
    ensure_ok(code, out, "truehdd CAF->WAV")

    ch_a, frames_a, data_a = read_wav_i32(ff12)
    ch_b, frames_b, data_b = read_wav_i32(ref12)
    if ch_a != 12 or ch_b != 12:
        raise RuntimeError(f"Expected 12ch compare, got ffmpeg={ch_a}, truehdd={ch_b}")

    frames = min(frames_a, frames_b)
    if frames_a != frames or frames_b != frames:
        data_a = array.array("i", data_a[: frames * 12])
        data_b = array.array("i", data_b[: frames * 12])

    mtx = corr_matrix(data_a, data_b, 12, frames)
    matches = greedy_match_abs(mtx)

    report = {
        "media": args.media,
        "stream_index": stream_index,
        "segment": {"ss": args.ss, "duration": args.duration},
        "files": {
            "thd": str(thd),
            "ffmpeg_12ch_wav": str(ff12),
            "truehdd_12ch_wav": str(ref12),
            "truehdd_metadata": str(ref_base) + ".atmos.metadata",
        },
        "ffmpeg_log_counts": {
            "oamd": count_pat(ff16_log, r"OAMD\["),
            "anomaly": count_pat(ff16_log, r"ANOMALY"),
            "degraded": count_pat(ff16_log, r"spatial substream degraded"),
            "anti_clip_adjusted": count_pat(ff16_log, r"anti-clip adjusted"),
            "lossless_failed": count_pat(ff16_log, r"Lossless check failed"),
        },
        "rms_dbfs": {
            "ffmpeg": per_ch_rms_dbfs(data_a, 12, frames),
            "truehdd": per_ch_rms_dbfs(data_b, 12, frames),
        },
        "corr_matrix": [[round(v, 6) for v in row] for row in mtx],
        "greedy_channel_match_abs_corr": matches,
    }

    text = json.dumps(report, indent=2)
    if args.json_out:
        Path(args.json_out).write_text(text, encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()
