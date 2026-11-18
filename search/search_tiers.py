#!/usr/bin/env python3
"""Selectable search scales （精密 / 標準 / 軽量） for the species mirror search.

One tier table drives both the candidate generator (this file) and
stage8/search_controller.py. Tier design: the standard and light tiers are
Sol's memo (stage8/gyarados/sol_scale_consult.md, adopted 2026-11-18);
the precision tier is the run3-class scale. All tiers sample from the
folded domain (definition A wasted points + definition B uniform spreads
are skipped during sampling) with the project's uniform scheme.

Usage:
  python3 search_tiers.py gen <species> <tier> <outdir>
Writes <outdir>/candidates.txt, panel.txt, candidates_meta.json and
run_config.json (consumed by search_controller.py).
"""
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import population3
from population import Stream

SAMPLE_SEED = 20263010
PANEL_SEED = 20263011

TIERS = {
    # 精密: run3-class full scale.
    'precision': {
        'label': '精密', 'candidates': 500_000, 'panel': 24,
        'a_rounds': [(10, 2_000)],
        'b': {'pool_extra': 4, 'games': 100, 'keep': 168},
        'finals_games': 300,
    },
    # 標準: Sol memo standard tier.
    'standard': {
        'label': '標準', 'candidates': 200_000, 'panel': 16,
        'a_rounds': [(4, 24_000), (8, 1_600)],
        'b': {'pool_extra': 8, 'games': 64, 'keep': 112},
        'finals_games': 240,
    },
    # 軽量: Sol memo light tier.
    'light': {
        'label': '軽量', 'candidates': 60_000, 'panel': 12,
        'a_rounds': [(3, 9_000), (7, 700)],
        'b': {'pool_extra': 6, 'games': 40, 'keep': 64},
        'finals_games': 160,
    },
    # Development fixture only (not a user tier): exercises the full
    # pipeline end-to-end in about a minute on a slow machine.
    'smoke': {
        'label': 'smoke', 'candidates': 2_000, 'panel': 4,
        'a_rounds': [(2, 200), (2, 50)],
        'b': {'pool_extra': 2, 'games': 4, 'keep': 8},
        'finals_games': 4,
    },
}


def tier_games(tier):
    """Total scheduled games for a tier (panel self-pairings included)."""
    t = TIERS[tier]
    total = 0
    n = t['candidates']
    for games, keep in t['a_rounds']:
        total += n * t['panel'] * games
        n = keep
    pool = t['panel'] + t['b']['pool_extra']
    total += n * pool * t['b']['games']
    k = t['b']['keep']
    total += k * k * t['finals_games']
    return total


def sample_builds(domain, seed, count, seen):
    rng = Stream(seed)
    profiles = domain.profiles
    out = []
    i = 0
    while len(out) < count:
        profile = profiles[i % len(profiles)]
        up, down = population3.NATURES[i % len(population3.NATURES)]
        i += 1
        while True:
            alloc = [rng.pick(33) for _ in range(6)]
            if sum(alloc) != 66 or max(alloc) <= 16:
                continue
            if population3.wasted_point(alloc, down, profile.base_stats):
                continue
            break
        pool = list(domain.moves_by_sex[profile.sex])
        moves = []
        for _ in range(4):
            moves.append(pool.pop(rng.pick(len(pool))))
        spec = (f"{domain.species}:{','.join(map(str, alloc))}|{','.join(sorted(moves))}"
                f"|obsh4|{profile.item}|{profile.mega}|{profile.ability}|{up},{down}|{profile.sex}")
        ident = population3.identity(spec)
        if ident in seen:
            continue
        seen.add(ident)
        out.append(spec)
    return out


def generate(species, tier, outdir):
    t = TIERS[tier]
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    catalog = outdir / 'population3_catalog'
    population3.build_catalog(catalog)
    domain = population3.load_domain(catalog, species)

    seen = set()
    panel = sample_builds(domain, PANEL_SEED, t['panel'], seen)
    rows = list(panel)
    n_forced = 0
    smoke_path = HERE / 'smoke_builds.json'
    if smoke_path.exists():
        smoke = json.loads(smoke_path.read_text(encoding='utf-8'))
        match = [b['spec'] for b in smoke['builds'] if b['species'] == species]
        if match:
            f = match[0].split('|')
            f[2] = 'obsh4'
            variants = ['|'.join(f)]
            stone = [p for p in domain.profiles if p.mega == 2]
            if stone:
                f2 = list(f)
                f2[3] = stone[0].item
                f2[4] = '2'
                variants.append('|'.join(f2))
            for spec in variants:
                ident = population3.identity(spec)
                if ident not in seen:
                    seen.add(ident)
                    rows.append(spec)
                    n_forced += 1
    sampled = sample_builds(domain, SAMPLE_SEED, t['candidates'] - len(rows), seen)
    rows.extend(sampled)

    (outdir / 'panel.txt').write_text('\n'.join(panel) + '\n', encoding='utf-8')
    payload = ('\n'.join(rows) + '\n').encode()
    (outdir / 'candidates.txt').write_bytes(payload)
    meta = {'species': species, 'tier': tier, 'tier_label': t['label'],
            'total': len(rows),
            'sources': {'panel': len(panel), 'forced': n_forced, 'sampled': len(sampled)},
            'sample_seed': SAMPLE_SEED, 'panel_seed': PANEL_SEED,
            'folds': ['definition B (max<=16 skipped)', 'definition A (wasted_point)'],
            'scheduled_games': tier_games(tier),
            'sha256': hashlib.sha256(payload).hexdigest()}
    (outdir / 'candidates_meta.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    (outdir / 'run_config.json').write_text(json.dumps(
        {'species': species, 'tier': tier}, indent=1), encoding='utf-8')
    print(json.dumps(meta, indent=1))


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == 'gen':
        generate(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        print(__doc__)
        for name, t in TIERS.items():
            print(f"{name:10s} ({t['label']}): candidates {t['candidates']}, "
                  f"scheduled games {tier_games(name)}")
