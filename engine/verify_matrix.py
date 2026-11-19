#!/usr/bin/env python3
"""Cross-platform matrix MD5 verifier (replaces md5sum dependency in Makefile).

Runs `engine matrix 3000 12345` in both plain (observer OFF) and compat
(`ENGINE_OBSERVER=1 ENGINE_OBS_COMPAT=1`) modes with conflicting env vars
cleared, strips CR (`\\r`), and checks MD5 against
`engine/fixtures/expected_matrix_md5.txt`.
"""
import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENGINE_DIR = ROOT / "engine"
FIXTURE = ENGINE_DIR / "fixtures" / "expected_matrix_md5.txt"


def find_engine_binary(explicit=None):
    if explicit:
        return Path(explicit)
    candidates = [
        ENGINE_DIR / ("engine.exe" if os.name == "nt" else "engine"),
        ENGINE_DIR / "engine.exe",
        ENGINE_DIR / "engine",
    ]
    for c in candidates:
        if c.exists():
            return c
    return candidates[0]


def clean_env():
    return {
        k: v for k, v in os.environ.items()
        if k not in ("ENGINE_SEQ", "ENGINE_OBSERVER", "ENGINE_OBS_COMPAT")
    }


def run_md5(engine_bin, extra_env):
    env = clean_env()
    env.update(extra_env)
    proc = subprocess.run(
        [str(engine_bin), "matrix", "3000", "12345"],
        cwd=ROOT,
        env=env,
        capture_output=True,
        check=True,
    )
    normalized = proc.stdout.replace(b"\r", b"")
    return hashlib.md5(normalized).hexdigest()


def main():
    ap = argparse.ArgumentParser(description="Verify engine matrix MD5 (cross-platform).")
    ap.add_argument("--engine", default=None, help="Path to compiled engine binary")
    ap.add_argument("--build", action="store_true", help="Compile engine/engine.c before verifying")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"), help="C compiler (default: gcc)")
    args = ap.parse_args()

    engine_bin = find_engine_binary(args.engine)
    if args.build or not engine_bin.exists():
        out_bin = ENGINE_DIR / ("engine.exe" if os.name == "nt" else "engine")
        cmd = [args.cc, "-O3", "-march=native", "-o", str(out_bin), str(ENGINE_DIR / "engine.c"), "-lm"]
        print("building:", " ".join(cmd), flush=True)
        subprocess.run(cmd, cwd=ROOT, check=True)
        engine_bin = out_bin

    want = FIXTURE.read_text(encoding="utf-8").strip()
    plain = run_md5(engine_bin, {})
    compat = run_md5(engine_bin, {"ENGINE_OBSERVER": "1", "ENGINE_OBS_COMPAT": "1"})

    print(f"plain:    {plain}")
    print(f"observer: {compat}")
    print(f"expected: {want}")

    if plain == want and compat == want:
        print("VERIFY OK")
        return 0
    print("VERIFY FAILED", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
