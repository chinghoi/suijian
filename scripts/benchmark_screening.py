#!/usr/bin/env python3
"""Compare two-pass and fused macOS I/O with the same legacy audio filter."""
import argparse
import hashlib
import json
import re
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="Video with an audio stream (never modified)")
    parser.add_argument("--start", type=float, default=60)
    parser.add_argument("--seconds", type=float, default=20)
    parser.add_argument("--ten-bit", action="store_true")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()
    if args.start < 0 or not 0 < args.seconds <= 120:
        parser.error("start must be nonnegative; seconds must be in (0, 120]")

    def run(kind):
        command = [args.ffmpeg, "-hide_banner", "-nostdin", "-nostats", "-ss", str(args.start)]
        if kind in ("hardware_video", "fused"):
            command += ["-hwaccel", "videotoolbox"]
        if kind == "fused":
            command += ["-hwaccel_output_format", "videotoolbox_vld"]
        command += ["-i", args.source]
        if kind != "audio":
            filters = "fps=2,"
            if kind == "fused":
                filters += "hwdownload,format=" + ("p010le" if args.ten_bit else "nv12") + ","
            filters += "scale=160:90:flags=fast_bilinear,format=gray"
            command += ["-t", str(args.seconds), "-map", "0:v:0", "-an", "-vf", filters,
                        "-f", "rawvideo", "pipe:1"]
        if kind in ("audio", "fused"):
            command += ["-t", str(args.seconds), "-map", "0:a:0", "-vn", "-af",
                        "silencedetect=noise=-30dB:d=0.8", "-f", "null", "-"]
        start = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=300, check=True)
        return {
            "seconds": round(time.monotonic() - start, 3),
            "frames": len(result.stdout) // 14400,
            "sha256": hashlib.sha256(result.stdout).hexdigest(),
            "silences": re.findall(r"silence_(?:start|end):\s*[-.0-9]+", result.stderr.decode()),
        }

    results = {}
    for kind in ("audio", "software_video", "hardware_video", "fused"):
        results[kind] = run(kind)
        print(json.dumps({kind: results[kind]}, ensure_ascii=False), flush=True)
    fused = results["fused"]
    if (not fused["frames"] or
            any(fused["sha256"] != results[k]["sha256"] for k in ("software_video", "hardware_video")) or
            fused["silences"] != results["audio"]["silences"]):
        raise SystemExit("FAIL: frame samples or silence boundaries changed")
    print("PASS: identical frame samples and silence boundaries")


if __name__ == "__main__":
    main()
