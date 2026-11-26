#!/usr/bin/env python3
"""Run Shelf's benchmark using the same fixture across binaries and repetitions."""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
from statistics import median
import struct
import subprocess
import tempfile
import time
import zlib


def fixtures(root):
    for count in (1000, 10000, 50000):
        folder = root / str(count)
        folder.mkdir()
        for n in range(count):
            (folder / f"file-{n}.txt").touch()
    for n in range(512):
        (root / "views" / str(n)).mkdir(parents=True)
    (root / "empty").mkdir()
    images = root / "images"
    images.mkdir()
    cache = root / "thumbnails"
    cache.mkdir()

    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))

    header = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">2I5B", 320, 180, 8, 2, 0, 0, 0))
    pixels = chunk(b"IDAT", zlib.compress((b"\x00" + b"\x22\x66\x99" * 320) * 180)) + chunk(b"IEND", b"")
    for n in range(64):
        file = images / f"{n}.png"
        file.write_bytes(header + pixels)
        uri = file.resolve().as_uri().encode()
        name = hashlib.md5(uri, usedforsecurity=False).hexdigest()
        mtime = str(int(file.stat().st_mtime)).encode()
        (cache / f"{name}.png").write_bytes(header + chunk(b"tEXt", b"Thumb::MTime\0" + mtime) + pixels)


def run(binary, root, watched):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software",
               HOME=str(watched))
    for key in ("CONFIG", "DATA", "STATE", "CACHE", "RUNTIME"):
        env[f"XDG_{key}_HOME" if key != "RUNTIME" else "XDG_RUNTIME_DIR"] = str(watched)
    def state():
        return {str(p.relative_to(watched)): (p.read_bytes() if p.is_file() else None) for p in watched.rglob("*")}

    original = state()
    proc = subprocess.run([str(binary.resolve()), "--bench", str(root.resolve())],
                          timeout=180, env=env, capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout.strip():
        raise RuntimeError(f"Shelf benchmark failed ({proc.returncode}): {proc.stderr}")
    if state() != original:
        raise RuntimeError("--bench wrote to the caller's HOME or XDG directories")
    if "ReferenceError:" in proc.stderr or "TypeError:" in proc.stderr:
        raise RuntimeError(proc.stderr)
    result = json.loads(proc.stdout.strip().splitlines()[-1])
    if result["first_files_frame_ms"] <= 0 or result["open_50000_ms"] <= 0:
        raise RuntimeError(f"Incomplete benchmark: {result}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--baseline-binary", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--fixture-parent", type=Path, help="put the fixture on a specific filesystem")
    parser.add_argument("--threshold", type=float, help="fail on a median regression above this percentage and 20 ms")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")

    def series(binary):
        runs = [run(binary, root, watched) for _ in range(args.runs)]
        metrics = {}
        for key in runs[0]:
            if key == "version":
                continue
            ordered = sorted(r[key] for r in runs)
            metrics[key] = {"median": median(ordered), "p95": ordered[math.ceil(len(ordered) * .95) - 1]}
        return {"version": runs[0]["version"], "runs": args.runs, "metrics": metrics}

    with tempfile.TemporaryDirectory(dir=args.fixture_parent) as temp:
        root = Path(temp) / "fixture"
        watched = Path(temp) / "caller"
        root.mkdir()
        watched.mkdir(mode=0o700)
        fixtures(root)
        old_session = watched / "shelf" / "sessions" / "old.json"
        old_session.parent.mkdir(parents=True)
        old_session.write_text(json.dumps({"pid": -1, "saved": int(time.time()), "paths": [str(root / "empty")]}))
        base = series(args.baseline_binary) if args.baseline_binary else None
        result = series(args.binary)
    if args.baseline:
        base = json.loads(args.baseline.read_text())
    failures = []
    print(f"{'metric':<36} {'median':>10} {'p95':>10} {'delta':>10}")
    for metric, values in sorted(result["metrics"].items()):
        before = base and base["metrics"].get(metric, {}).get("median")
        delta = (values["median"] / before - 1) * 100 if before and before > 0 else None
        difference = f"{delta:+.1f}%" if delta is not None else ""
        print(f"{metric:<36} {values['median']:>10.1f} {values['p95']:>10.1f} {difference:>10}")
        if (args.threshold is not None and delta is not None and metric.endswith("_ms")
                and delta > args.threshold and values["median"] - before > 20):
            failures.append(metric)
    if args.output:
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    if failures:
        raise SystemExit("Performance regression: " + ", ".join(failures))


if __name__ == "__main__":
    main()
