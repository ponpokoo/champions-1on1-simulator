#!/usr/bin/env python3
"""Tier-driven species search controller (companion to search_tiers.py).

Reads run_config.json in the run directory (written by the generator) and
executes: build -> gate (matrix MD5 both modes + 1-game screen) -> stage A
rounds (cumulative per-opponent W/L/D across rounds) -> stage B (fresh
measurement against panel + top extra builds) -> finals round-robin.
Markers (<step>.done) make a restarted controller resume; FAIL.txt on error.

Usage: python3 search/search_controller.py <rundir> [--gcc <path>] [--shards N]
The engine binary is built as <repo root>/engine_search.exe (Windows) or
engine_search (elsewhere). Ranking: mean score, worst pairing tie-break.
"""
import argparse
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
# Minimum scheduled games per shard before another shard is worth spawning.
GAMES_PER_SHARD_MIN = 5_000


def log(msg):
    print(msg, flush=True)


class Controller:
    def __init__(self, rundir, gcc, shards):
        self.dir = Path(rundir)
        self.gcc = gcc
        self.shards = shards
        cfg = json.loads((self.dir / 'run_config.json').read_text(encoding='utf-8'))
        self.species = cfg.get('species', '')
        self.filters = cfg.get('filters')
        self.tier = TIERS[cfg['tier']]
        self.tier_name = cfg['tier']
        meta_path = self.dir / 'candidates_meta.json'
        self.candidates_meta = json.loads(meta_path.read_text(encoding='utf-8')) if meta_path.exists() else {}
        engine_name = 'engine_search.exe' if os.name == 'nt' else 'engine_search'
        self.engine = ROOT / engine_name

    def screenx(self, cand_file, pool_file, games, seed, out_file):
        with open(out_file, 'w', encoding='utf-8') as out:
            p = subprocess.run([str(self.engine), 'screenx', str(cand_file), str(pool_file),
                                str(games), str(seed)], cwd=ROOT, stdout=out,
                               stderr=subprocess.PIPE, text=True, encoding='utf-8',
                               errors='replace')
        if p.returncode != 0:
            raise RuntimeError(f'screenx failed rc={p.returncode}: {p.stderr[-500:]}')

    def run_shards(self, cand_lines, pool_file, games, seed0, tag):
        # Shard count follows the actual workload (candidates x pool x games),
        # not just the candidate count, so heavy stages with few candidates
        # (e.g. finals: 64 x 64 x 160 games) still run in parallel. The legacy
        # candidates // 50 rule is kept as a floor so no stage loses shards.
        pool_size = sum(1 for ln in Path(pool_file).read_text(encoding='utf-8').splitlines()
                        if ln.strip())
        total_games = len(cand_lines) * pool_size * games
        nshards = min(self.shards, max(1, len(cand_lines)),
                      max(1, len(cand_lines) // 50, total_games // GAMES_PER_SHARD_MIN))
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
            for line in (self.dir / f'{tag}_out_{s:02d}.txt').read_text(encoding='utf-8').splitlines():
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

    @staticmethod
    def rank_both_seats(rows):
        n = len(rows)
        if n <= 1:
            return [(0.5, 0.5, 0)] if n == 1 else []
        out = []
        for i in range(n):
            scores = []
            for j in range(n):
                if i == j:
                    continue
                wa, la, da = rows[i][3 * j:3 * j + 3]
                sa = (wa + 0.5 * da) / (wa + la + da)
                wb, lb, db = rows[j][3 * i:3 * i + 3]
                sb = (lb + 0.5 * db) / (wb + lb + db)
                scores.append(0.5 * (sa + sb))
            out.append((sum(scores) / len(scores), min(scores), i))
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
        (self.dir / f'{name}.done').write_text('ok\n', encoding='utf-8')

    def run(self):
        d = self.dir
        candidates = (d / 'candidates.txt').read_text(encoding='utf-8').splitlines()
        panel = (d / 'panel.txt').read_text(encoding='utf-8').splitlines()
        log(f"tier {self.tier_name} ({self.tier['label']}): "
            f"candidates {len(candidates)}, panel {len(panel)}")

        if self.step('build'):
            p = subprocess.run([self.gcc, '-O3', '-march=native', '-o', str(self.engine),
                                'engine/engine.c', '-lm'], cwd=ROOT, capture_output=True,
                               text=True, encoding='utf-8', errors='replace')
            if p.returncode != 0:
                raise RuntimeError(f'build failed: {p.stderr[-800:]}')
            self.mark('build')

        if self.step('gate'):
            base_env = {k: v for k, v in os.environ.items()
                        if k not in ('ENGINE_SEQ', 'ENGINE_OBSERVER', 'ENGINE_OBS_COMPAT')}
            for mode, extra in (('regular', {}),
                                ('compat', {'ENGINE_OBSERVER': '1', 'ENGINE_OBS_COMPAT': '1'})):
                env = dict(base_env, **extra)
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
                surv = json.loads((d / f'{name}_survivors.json').read_text(encoding='utf-8'))
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
        ranking_b = json.loads((d / 'stageB_ranking.json').read_text(encoding='utf-8'))

        if self.step('finals'):
            finalists = [candidates[r['idx']] for r in ranking_b[:b['keep']]]
            (d / 'finalists.txt').write_text('\n'.join(finalists) + '\n', encoding='utf-8')
            rows = self.run_shards(finalists, d / 'finalists.txt',
                                   self.tier['finals_games'], SEED_F, 'finals')
            ranking = self.rank(rows, len(finalists))
            ranking_both = self.rank_both_seats(rows)
            matrix = []
            for fi, flat in enumerate(rows):
                matrix.append([round((flat[3 * p] + 0.5 * flat[3 * p + 2])
                                     / self.tier['finals_games'], 4)
                               for p in range(len(finalists))])
            result = {'species': self.species,
                      'tier': self.tier_name,
                      'tier_label': self.tier['label'],
                      'policy': 'obsh4',
                      'total_candidates': len(candidates),
                      'panel_size': len(panel),
                      'filters': self.filters,
                      'enumeration_mode': self.candidates_meta.get('enumeration_mode'),
                      'finals_games_per_pair': self.tier['finals_games'],
                      'finalists': finalists,
                      'ranking': [{'finalist': i, 'mean': m, 'worst': w}
                                  for m, w, i in ranking],
                      'ranking_both_seats': [{'finalist': i, 'mean': m, 'worst': w}
                                             for m, w, i in ranking_both],
                      'matrix': matrix}
            (d / 'finals_results.json').write_text(
                json.dumps(result, ensure_ascii=False), encoding='utf-8'
            )
            log(f"finals #1 (seat A): finalist {ranking[0][2]} mean {ranking[0][0]:.4f} "
                f"worst {ranking[0][1]:.4f}")
            log(f"finals #1 (both seats, no mirror): finalist {ranking_both[0][2]} "
                f"mean {ranking_both[0][0]:.4f} worst {ranking_both[0][1]:.4f}")
            self.mark('finals')

        finals_data = json.loads((d / 'finals_results.json').read_text(encoding='utf-8'))
        top_a = finals_data['ranking'][0]
        both_list = finals_data.get('ranking_both_seats')
        summary = {
            'status': 'complete',
            'species': self.species,
            'tier': self.tier_name,
            'tier_label': self.tier['label'],
            'policy': 'obsh4',
            'candidates': len(candidates),
            'panel_size': len(panel),
            'filters': self.filters,
            'enumeration_mode': self.candidates_meta.get('enumeration_mode'),
            'finalists': len(finals_data['finalists']),
            'finals_games_per_pair': self.tier['finals_games'],
            'top1_seat_a': {
                'spec': finals_data['finalists'][top_a['finalist']],
                'mean': top_a['mean'],
                'worst': top_a['worst'],
            },
        }
        if both_list:
            top_both = both_list[0]
            summary['top1_both_seats'] = {
                'spec': finals_data['finalists'][top_both['finalist']],
                'mean': top_both['mean'],
                'worst': top_both['worst'],
            }
        (d / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False) + '\n',
                                        encoding='utf-8')
        (d / 'DONE').write_text('done\n', encoding='utf-8')
        log('ALL DONE')


def main():
    parser = argparse.ArgumentParser(description='Run tier-driven species 1on1 mirror search.')
    parser.add_argument('rundir', help='Run directory created by search_tiers.py gen')
    parser.add_argument('--gcc', default='gcc', help='C compiler path (default: gcc)')
    parser.add_argument('--shards', type=int, default=10, help='Parallel shard workers (default: 10)')
    args = parser.parse_args()
    if args.shards < 1:
        parser.error('--shards must be >= 1')
    ctl = Controller(args.rundir, args.gcc, args.shards)
    try:
        ctl.run()
    except Exception:
        (Path(args.rundir) / 'FAIL.txt').write_text(traceback.format_exc(), encoding='utf-8')
        raise


if __name__ == '__main__':
    main()
