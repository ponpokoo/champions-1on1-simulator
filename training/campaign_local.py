#!/usr/bin/env python3
"""Local campaign controller for the v1 (MGO1 / 17-move Metagross) RL loop.

Runs the end-to-end self-play loop on one machine:
  1) Build training/loop and training/parity if needed (gcc)
  2) Prepare deterministic 176-build v1 train & held-out populations if omitted
  3) Initialize bootstrap.bin if --model does not exist
  4) For each generation:
     - Generate MGT3 self-play trajectories with loop (CPU)
     - Train PPO policy/value network with train.py (GPU, or --cpu-smoke)
     - Verify C vs PyTorch parity with parity.py
  5) Evaluate final model on the held-out (or specified) population and summarize

Note: This training pipeline targets the frozen v1 Metagross 17-move schema
(MGO1). For GPU offloading on Colab, run steps 1-4 up to trajectory generation
locally, run train.py on Colab per training/colab/README.md, and run parity.py
+ evaluate locally.
"""
import argparse
import hashlib
import json
import math
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRAINING = ROOT / "training"

V1_MOVES = (
    "BP", "BD", "EQ", "SCL", "SB", "MS", "AGI", "CP", "ID",
    "PRO", "PU", "REST", "SF", "ST", "SUB", "TRK", "PN",
)
V1_ITEMS = (
    "none", "leftovers", "sitrus", "scarf", "lifeorb", "muscle", "wise",
    "belt", "tag", "sand", "spoon", "bbelt", "metronome", "shellbell",
    "lens", "kings", "wide", "zoom", "powder", "claw", "ironball",
    "shuca", "chople", "kasib", "payapa", "chesto", "lum", "leppa",
    "oran", "sash", "fband", "balloon", "herb", "helmet",
)
CORES = (
    (155, 155, 150, 115, 110, 90),   # Metagross normal (Lv.50, IV31, 0 pts)
    (155, 165, 170, 125, 130, 130),  # Mega Metagross (Lv.50, IV31, 0 pts)
)
NATURES = [(0, 0)] + [(u, d) for u in range(1, 6) for d in range(1, 6) if u != d]
MASK64 = (1 << 64) - 1


def _hash64(z):
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


class _Stream:
    def __init__(self, seed):
        self.state = seed

    def pick(self, n):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        return _hash64(self.state) % n


def _v1_identity(spec):
    f = spec.split("|")
    return f[0], tuple(sorted(f[1].split(","))), f[3], f[4], f[5] if f[4] == "0" else "tc"


def generate_v1_population_pair(train_path, heldout_path, seed=20261600, extra_exclude=()):
    """Generate disjoint 176-build OBS_REGULAR v1 Metagross populations."""
    rng = _Stream(seed)
    seen = {_v1_identity(s) for s in extra_exclude}

    def build_176():
        rows = []
        while len(rows) < 176:
            i = len(rows)
            mega = i % 2
            while True:
                alloc = [rng.pick(33) for _ in range(6)]
                if sum(alloc) == 66 and max(alloc) > 16:
                    break
            up, down = NATURES[i % len(NATURES)]
            stats = []
            for idx, (base, pt) in enumerate(zip(CORES[mega], alloc)):
                v = base + pt
                if up and idx == up:
                    v = math.floor(v * 1.1)
                elif down and idx == down:
                    v = math.floor(v * 0.9)
                stats.append(v)
            pool = list(V1_MOVES)
            moves = [pool.pop(rng.pick(len(pool))) for _ in range(4)]
            item = "stone" if mega else V1_ITEMS[(i // 2) % len(V1_ITEMS)]
            ability = "cb" if (mega or (i // 2) % 2 == 0) else "lm"
            spec = f"{','.join(map(str, stats))}|{','.join(moves)}|generic|{item}|{mega}|{ability}"
            key = _v1_identity(spec)
            if key in seen:
                continue
            seen.add(key)
            rows.append(spec)
        return rows

    train_rows = build_176()
    heldout_rows = build_176()
    if train_path is not None:
        Path(train_path).parent.mkdir(parents=True, exist_ok=True)
        Path(train_path).write_text("\n".join(train_rows) + "\n", encoding="utf-8")
    if heldout_path is not None:
        Path(heldout_path).parent.mkdir(parents=True, exist_ok=True)
        Path(heldout_path).write_text("\n".join(heldout_rows) + "\n", encoding="utf-8")


def read_mwp1_sha256(path):
    data = Path(path).read_bytes()
    if len(data) < 16 or struct.unpack("<4sIII", data[:16]) != (b"MWP1", 415, 64, 32):
        raise ValueError(f"invalid MWP1 model file: {path}")
    return hashlib.sha256(data[16:]).hexdigest()


def run(cmd):
    print("RUN", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run([str(c) for c in cmd], check=True, cwd=ROOT)


def ensure_binaries(gcc):
    exe_suffix = ".exe" if os.name == "nt" else ""
    loop_bin = TRAINING / f"loop{exe_suffix}"
    parity_bin = TRAINING / f"parity{exe_suffix}"
    if not loop_bin.exists():
        run([gcc, "-O3", "-Wall", "-Wextra", "-o", loop_bin, TRAINING / "loop.c", "-lm"])
    if not parity_bin.exists():
        run([gcc, "-O3", "-Wall", "-Wextra", "-o", parity_bin, TRAINING / "parity.c", "-lm"])
    return loop_bin, parity_bin


def main():
    ap = argparse.ArgumentParser(description="Run local v1 (MGO1) PPO training campaign.")
    ap.add_argument("--workdir", default=str(TRAINING / "work"), help="Working directory for runs")
    ap.add_argument("--gcc", default="gcc", help="C compiler path (default: gcc)")
    ap.add_argument("--generations", type=int, default=3)
    ap.add_argument("--games-per-gen", type=int, default=12000)
    ap.add_argument("--eval-games", type=int, default=500, help="Games per pairing/seat in final eval")
    ap.add_argument("--epochs", type=int, default=6)
    ap.add_argument("--seconds", type=int, default=540)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--kl-stop", type=float, default=0.03)
    ap.add_argument("--seed", type=int, default=20261601)
    ap.add_argument("--population", default="", help="176-build v1 training population file")
    ap.add_argument("--eval-population", default="", help="176-build v1 held-out evaluation file")
    ap.add_argument("--model", default="", help="Initial MWP1 .bin model (initialized if omitted)")
    ap.add_argument("--cpu-smoke", action="store_true", help="Run small CPU smoke (<=256 games/gen)")
    args = ap.parse_args()

    work = Path(args.workdir)
    work.mkdir(parents=True, exist_ok=True)

    if args.population and not Path(args.population).exists():
        raise SystemExit(f"population file not found: {args.population}")
    if args.eval_population and not Path(args.eval_population).exists():
        raise SystemExit(f"eval population file not found: {args.eval_population}")

    pop_path = Path(args.population) if args.population else work / "population_train.txt"
    eval_pop_path = Path(args.eval_population) if args.eval_population else work / "population_heldout.txt"
    need_train = not args.population and not pop_path.exists()
    need_eval = not args.eval_population and not eval_pop_path.exists()
    if need_train or need_eval:
        extra = pop_path.read_text(encoding="utf-8").splitlines() if pop_path.exists() else ()
        generate_v1_population_pair(
            pop_path if need_train else None,
            eval_pop_path if need_eval else None,
            seed=args.seed - 1,
            extra_exclude=extra,
        )
        print(f"prepared default v1 population(s): {pop_path}, {eval_pop_path}", flush=True)

    loop_bin, parity_bin = ensure_binaries(args.gcc)

    model = Path(args.model) if args.model else work / "models" / "bootstrap.bin"
    if not model.exists():
        model.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, TRAINING / "model.py", "init", model])

    snapshots = []
    for gen in range(1, args.generations + 1):
        traj = work / "traj" / f"g{gen:02d}.mgt"
        traj.parent.mkdir(parents=True, exist_ok=True)
        digest = read_mwp1_sha256(model)

        # 1) self-play trajectory generation (CPU)
        gen_cmd = [
            loop_bin, "stage3", "generate", pop_path, traj,
            args.games_per_gen, args.seed + gen, model, digest,
            *snapshots[-16:],
        ]
        run(gen_cmd)

        # 2) PPO training (GPU, or --cpu-smoke for <=256 games)
        out_model = work / "models" / f"g{gen:02d}.bin"
        out_model.parent.mkdir(parents=True, exist_ok=True)
        train_cmd = [
            sys.executable, TRAINING / "train.py",
            "--data", traj, "--input", model, "--output", out_model,
            "--epochs", args.epochs, "--seconds", args.seconds,
            "--lr", args.lr, "--kl-stop", args.kl_stop, "--seed", args.seed + 100 + gen,
        ]
        if args.cpu_smoke:
            train_cmd.append("--cpu-smoke")
        run(train_cmd)

        # 3) C vs PyTorch inference parity check
        run([
            sys.executable, TRAINING / "parity.py",
            out_model, f"{out_model}.parity.npz", "--executable", parity_bin,
        ])

        print(json.dumps({"generation": gen, "model": str(out_model)}), flush=True)
        snapshots.append(model)
        model = out_model

    # 4) Deterministic held-out evaluation
    eval_prefix = work / "eval" / "final"
    eval_prefix.parent.mkdir(parents=True, exist_ok=True)
    run([
        loop_bin, "stage3", "evaluate", eval_pop_path, eval_prefix,
        args.eval_games, args.seed + 1000, model,
    ])
    run([sys.executable, TRAINING / "analyze.py", eval_prefix])


if __name__ == "__main__":
    main()
