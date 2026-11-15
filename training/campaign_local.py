"""Local campaign controller (reference implementation).

Runs the self-play learning loop on one machine:
  generate trajectories with the engine-side loop binary (CPU)
  -> train with train.py (PyTorch; GPU recommended)
  -> export weights as a C header
  -> deterministic evaluation against fixed benchmarks

The original development ran the training leg on a separate GPU PC; this
local version keeps the same phase structure so the loop can be driven from
a single checkout, or split again by moving only the train step to Colab /
Modal. Evaluation must use held-out builds to say anything about
generalization; see training/README.md.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TRAINING = ROOT / "training"


def run(cmd, **kw):
    print("RUN", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, cwd=ROOT, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--generations", type=int, default=3)
    ap.add_argument("--games-per-gen", type=int, default=12000)
    ap.add_argument("--seed", type=int, default=20261601)
    ap.add_argument("--population", default=str(TRAINING / "population.txt"))
    ap.add_argument("--model", default=str(TRAINING / "models" / "bootstrap.bin"))
    args = ap.parse_args()

    model = args.model
    for gen in range(1, args.generations + 1):
        traj = TRAINING / "traj" / f"g{gen:02d}.mgt"
        traj.parent.mkdir(parents=True, exist_ok=True)
        # 1) self-play trajectory generation (CPU; loop binary built from loop.c)
        run([TRAINING / "loop", "generate", args.population, traj,
             args.games_per_gen, args.seed + gen, model])
        # 2) PPO training (GPU recommended)
        out_model = TRAINING / "models" / f"g{gen:02d}.bin"
        out_model.parent.mkdir(parents=True, exist_ok=True)
        run([sys.executable, TRAINING / "train.py",
             "--data", traj, "--init", model, "--out", out_model])
        # 3) export + evaluate are handled by train.py's export step and the
        #    engine's deterministic evaluation modes; record the checkpoint.
        print(json.dumps({"generation": gen, "model": str(out_model)}))
        model = str(out_model)


if __name__ == "__main__":
    main()
