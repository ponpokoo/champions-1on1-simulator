#!/usr/bin/env python3
"""Tier-driven species search controller (companion to search_tiers.py).

Reads run_config.json in the run directory (written by the generator) and
executes: build -> gate (matrix MD5 both modes + 1-game screen) -> stage A
rounds (cumulative per-opponent W/L/D across rounds) -> stage B (fresh
measurement against panel + top extra builds) -> finals round-robin.
Markers (<step>.done) make a restarted controller resume; FAIL.txt on error.

Usage: python3 search_controller.py <rundir> [--gcc <path>] [--shards N]
The engine binary is built as <repo root>/engine_search.exe (Windows) or
engine_search (elsewhere). Ranking: mean score, worst pairing tie-break.
"""
import hashlib
import json
import os
import subprocess
import sys
import time
import traceback
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from search_tiers import TIERS

MATRIX_MD5 = 'f3128c506954574a0c48bdfb79e2a917'
SEED_A0, SEED_B, SEED_F = 20263310, 20263350, 20263360


def log(msg):
    print(msg, flush=True)


class Controller:
    def __init__(self, rundir, gcc, shards):
        self.dir = Path(rundir)
        self.gcc = gcc
        self.shards = shards
        cfg = json.loads((self.dir / 'run_config.json').read_text())
        self.tier = TIERS[cfg['tier']]
        self.tier_name = cfg['tier']
        engine_name = 'engine_search.exe' if os.name == 'nt' else 'engine_search'
        self.engine = ROOT / engine_name

    def screenx(self, cand_file, pool_file, games, seed, out_file):
        with open(out_file, 'w') as out:
            p = subprocess.run([str(self.engine), 'screenx', str(cand_file), str(pool_file),
                                str(games), str(seed)], cwd=ROOT, stdout=out,
                               stderr=subprocess.PIPE, text=True)
        if p.returncode != 0:
            raise RuntimeError(f'screenx failed rc={p.returncode}: {p.stderr[-500:]}')

    def run_shards(self, cand_lines, pool_file, games, seed0, tag):
        nshards = min(self.shards, max(1, len(cand_lines) // 50))
        def shard_file(lines, idx, n):
            f = self.dir / f'{tag}_cand_{idx:02d}.txt'
            f.write_text('\n'.join(lines[idx::n]) + '\n', encoding='utf-8')
            return f
        t0 = time.time()
        with ThreadPoolExecutor(max_workers=nshards) as ex:
            futs = {}
            for s in range(nshards):
                cf = shard_file(cand_lines, s, nshards)
                of = self.dir / f'{tag}_out_{s:02d}.txt'
                futs[ex.submit(self.screenx, cf, pool_file, games, seed0 + s, of)] = s
            done_n = 0
            for f in as_completed(futs):
                f.result()
                done_n += 1
                el = time.time() - t0
                eta = el / done_n * (nshards - done_n) if done_n < nshards else 0.0
                log(f'[progress] {tag} shards {done_n}/{nshards} '
                    f'({100.0 * done_n / nshards:.0f}%) elapsed {el:.0f}s eta {eta:.0f}s')
        merged = {}
        for s in range(nshards):
            for line in (self.dir / f'{tag}_out_{s:02d}.txt').read_text().splitlines():
                parts = list(map(int, line.split()))
                merged[s + parts[0] * nshards] = parts[1:]
        return [merged[i] for i in sorted(merged)]

    @staticmethod
    def rank(rows, pool_size, keys=None):
        out = []
        for pos, flat in enumerate(rows):
            scores = []
            for p in range(pool_size):
                w, l, d = flat[3 * p:3 * p + 3]
                scores.append((w + 0.5 * d) / (w + l + d))
            out.append((sum(scores) / pool_size, min(scores), keys[pos] if keys else pos))
        out.sort(key=lambda t: (-t[0], -t[1], t[2]))
        return out

    def step(self, name):
        marker = self.dir / f'{name}.done'
        if marker.exists():
            log(f'skip {name} (done)')
            return False
        log(f'step {name} ...')
        return True

    def mark(self, name):
        (self.dir / f'{name}.done').write_text('ok\n')

    def run(self):
        d = self.dir
        candidates = (d / 'candidates.txt').read_text(encoding='utf-8').splitlines()
        panel = (d / 'panel.txt').read_text(encoding='utf-8').splitlines()
        log(f"tier {self.tier_name} ({self.tier['label']}): "
            f"candidates {len(candidates)}, panel {len(panel)}")

        if self.step('build'):
            p = subprocess.run([self.gcc, '-O3', '-march=native', '-o', str(self.engine),
                                'engine/engine.c', '-lm'], cwd=ROOT, capture_output=True, text=True)
            if p.returncode != 0:
                raise RuntimeError(f'build failed: {p.stderr[-800:]}')
            self.mark('build')

        if self.step('gate'):
            for mode, extra in (('regular', {}),
                                ('compat', {'ENGINE_OBSERVER': '1', 'ENGINE_OBS_COMPAT': '1'})):
                env = dict(os.environ, **extra)
                p = subprocess.run([str(self.engine), 'matrix', '3000', '12345'], cwd=ROOT,
                                   env=env, capture_output=True)
                digest = hashlib.md5(p.stdout.replace(b'\r\n', b'\n')).hexdigest()
                log(f'gate matrix {mode}: {digest}')
                if digest != MATRIX_MD5:
                    raise RuntimeError(f'matrix MD5 mismatch ({mode}): {digest}')
            one = d / 'gate_one.txt'
            one.write_text(candidates[0] + '\n', encoding='utf-8')
            self.screenx(one, d / 'panel.txt', 1, 20263300, d / 'gate_out.txt')
            self.mark('gate')

        surv_idx = list(range(len(candidates)))
        surv_rows = None
        for r, (games, keep) in enumerate(self.tier['a_rounds'], start=1):
            name = f'stageA{r}'
            if self.step(name):
                specs = [candidates[i] for i in surv_idx]
                rows = self.run_shards(specs, d / 'panel.txt', games,
                                       SEED_A0 + 10 * (r - 1), name)
                if surv_rows is not None:
                    rows = [[x + y for x, y in zip(surv_rows[pos], rows[pos])]
                            for pos in range(len(rows))]
                by_idx = {surv_idx[pos]: rows[pos] for pos in range(len(rows))}
                ranking = self.rank(rows, len(panel), keys=surv_idx)
                (d / f'{name}_ranking.json').write_text(json.dumps(
                    [{'idx': i, 'mean': m, 'worst': w} for m, w, i in ranking]), encoding='utf-8')
                surv_idx = [i for _, _, i in ranking[:keep]]
                surv_rows = [by_idx[i] for i in surv_idx]
                (d / f'{name}_survivors.json').write_text(json.dumps(
                    {'idx': surv_idx, 'rows': {str(i): by_idx[i] for i in surv_idx}}),
                    encoding='utf-8')
                log(f'{name} top: {ranking[0]}')
                self.mark(name)
            else:
                surv = json.loads((d / f'{name}_survivors.json').read_text())
                surv_idx = surv['idx']
                surv_rows = [surv['rows'][str(i)] for i in surv_idx]

        b = self.tier['b']
        if self.step('stageB'):
            pool2 = list(panel)
            for i in surv_idx:
                if candidates[i] not in pool2 and len(pool2) < len(panel) + b['pool_extra']:
                    pool2.append(candidates[i])
            (d / 'pool2.txt').write_text('\n'.join(pool2) + '\n', encoding='utf-8')
            cand_b = [candidates[i] for i in surv_idx]
            rows = self.run_shards(cand_b, d / 'pool2.txt', b['games'], SEED_B, 'stageB')
            ranking = self.rank(rows, len(pool2), keys=surv_idx)
            (d / 'stageB_ranking.json').write_text(json.dumps(
                [{'idx': i, 'mean': m, 'worst': w} for m, w, i in ranking]), encoding='utf-8')
            log(f'stageB top: {ranking[0]}')
            self.mark('stageB')
        ranking_b = json.loads((d / 'stageB_ranking.json').read_text())

        if self.step('finals'):
            finalists = [candidates[r['idx']] for r in ranking_b[:b['keep']]]
            (d / 'finalists.txt').write_text('\n'.join(finalists) + '\n', encoding='utf-8')
            rows = self.run_shards(finalists, d / 'finalists.txt',
                                   self.tier['finals_games'], SEED_F, 'finals')
            ranking = self.rank(rows, len(finalists))
            matrix = []
            for fi, flat in enumerate(rows):
                matrix.append([round((flat[3 * p] + 0.5 * flat[3 * p + 2])
                                     / self.tier['finals_games'], 4)
                               for p in range(len(finalists))])
            result = {'finalists': finalists,
                      'ranking': [{'finalist': i, 'mean': m, 'worst': w}
                                  for m, w, i in ranking],
                      'matrix': matrix}
            (d / 'finals_results.json').write_text(json.dumps(result), encoding='utf-8')
            log(f"finals #1: finalist {ranking[0][2]} mean {ranking[0][0]:.4f} "
                f"worst {ranking[0][1]:.4f}")
            self.mark('finals')

        (d / 'summary.json').write_text(json.dumps(
            {'status': 'complete', 'tier': self.tier_name}), encoding='utf-8')
        (d / 'DONE').write_text('done\n')
        log('ALL DONE')


def main():
    args = sys.argv[1:]
    rundir = args[0]
    gcc = 'gcc'
    shards = 10
    for i, a in enumerate(args):
        if a == '--gcc':
            gcc = args[i + 1]
        if a == '--shards':
            shards = int(args[i + 1])
    ctl = Controller(rundir, gcc, shards)
    try:
        ctl.run()
    except Exception:
        (Path(rundir) / 'FAIL.txt').write_text(traceback.format_exc(), encoding='utf-8')
        raise


if __name__ == '__main__':
    main()
