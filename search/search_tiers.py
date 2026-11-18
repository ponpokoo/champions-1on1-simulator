#!/usr/bin/env python3
"""Selectable search scales (精密 / 標準 / 軽量) for the species mirror search.

One tier table drives both the candidate generator (this file) and
search/search_controller.py. All tiers sample from the folded domain
(definition A wasted points + definition B uniform spreads are skipped during
sampling) with the project's uniform scheme. Optional explicit subdomain filters
(--move, --require-move, --item, --mega, --ability, --sex, --nature,
--allocation, --stat-min, --stat-max, --move-count) narrow candidate generation
while keeping the opponent panel unconstrained across the whole species.

Usage:
  python3 search/search_tiers.py gen <species> <tier> <outdir> [options]
  python3 search/search_tiers.py list
Writes <outdir>/candidates.txt, panel.txt, candidates_meta.json and
run_config.json (consumed by search/search_controller.py).
"""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import population3
from population3 import Stream

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
    # 標準: balanced default tier.
    'standard': {
        'label': '標準', 'candidates': 200_000, 'panel': 16,
        'a_rounds': [(4, 24_000), (8, 1_600)],
        'b': {'pool_extra': 8, 'games': 64, 'keep': 112},
        'finals_games': 240,
    },
    # 軽量: fast screening tier.
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


def _matches_filters(spec, filters):
    if not filters:
        return True
    fields = spec.strip().split('|')
    _, pts_s = fields[0].split(':', 1)
    pts = tuple(map(int, pts_s.split(',')))
    moves = set(fields[1].split(','))
    item = fields[3]
    mega = int(fields[4])
    ability = fields[5]
    nature = tuple(map(int, fields[6].split(',')))
    sex = fields[7]
    if filters.get('allocations') and pts not in set(map(tuple, filters['allocations'])):
        return False
    if filters.get('stat_min') and any(pts[k] < filters['stat_min'][k] for k in range(6)):
        return False
    if filters.get('stat_max') and any(pts[k] > filters['stat_max'][k] for k in range(6)):
        return False
    if filters.get('natures') and nature not in set(map(tuple, filters['natures'])):
        return False
    if filters.get('move_pool') and not moves.issubset(set(filters['move_pool'])):
        return False
    if filters.get('required_moves') and not set(filters['required_moves']).issubset(moves):
        return False
    if filters.get('move_counts') and len(moves) not in set(filters['move_counts']):
        return False
    if filters.get('items') and item not in set(filters['items']):
        return False
    if filters.get('megas') is not None and mega not in set(filters['megas']):
        return False
    if filters.get('abilities') and ability not in set(filters['abilities']):
        return False
    if filters.get('sexes') and sex not in set(filters['sexes']):
        return False
    return True


def generate(species, tier, outdir, gcc='gcc', filters=None, max_candidates=None):
    t = TIERS[tier]
    target_candidates = max_candidates or t['candidates']
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    catalog = outdir / ('population3_catalog.exe' if os.name == 'nt' else 'population3_catalog')
    population3.build_catalog(catalog, cc=gcc)
    domain = population3.load_domain(catalog, species)

    # Opponent panel is always drawn from the unconstrained full species domain.
    panel_seen = set()
    panel = sample_builds(domain, PANEL_SEED, t['panel'], panel_seen)

    has_filters = bool(filters and any(v is not None for v in filters.values()))
    if not has_filters and max_candidates is None:
        seen = set(panel_seen)
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
        sampled = sample_builds(domain, SAMPLE_SEED, target_candidates - len(rows), seen)
        rows.extend(sampled)
        enumeration_mode = 'sampled'
        space_size = None
        exact_space = False
    else:
        f = filters or {}
        kw = dict(
            allocations=f.get('allocations'),
            stat_min=f.get('stat_min'),
            stat_max=f.get('stat_max'),
            natures=f.get('natures') or population3.NATURES,
            move_pool=f.get('move_pool'),
            required_moves=f.get('required_moves'),
            items=f.get('items'),
            megas=f.get('megas'),
            abilities=f.get('abilities'),
            sexes=f.get('sexes'),
            move_counts=f.get('move_counts') or (4,),
        )
        space_size, exact_space = population3.estimate_filtered_count(domain, **kw)
        if space_size == 0:
            raise ValueError(f'{species}: 指定された絞り込み条件に一致する合法な型がありません')
        seen = set()
        rows = []
        n_forced = 0
        # Include unconstrained panel specs only if they also satisfy the filter.
        for spec in panel:
            if _matches_filters(spec, f):
                ident = population3.identity(spec)
                if ident not in seen:
                    seen.add(ident)
                    rows.append(spec)
        if exact_space and space_size <= target_candidates:
            enumeration_mode = 'exhaustive'
            for spec in population3.candidates(domain, **kw):
                ident = population3.identity(spec)
                if ident not in seen:
                    seen.add(ident)
                    rows.append(spec)
            sampled = []
        else:
            enumeration_mode = 'sampled'
            need = max(0, target_candidates - len(rows))
            sampled = population3.sample_filtered_builds(domain, SAMPLE_SEED, need, seen, **kw)
            rows.extend(sampled)
            if not rows:
                raise ValueError(f'{species}: 指定された絞り込み条件に一致する合法な型が見つかりませんでした')

    (outdir / 'panel.txt').write_text('\n'.join(panel) + '\n', encoding='utf-8')
    payload = ('\n'.join(rows) + '\n').encode('utf-8')
    (outdir / 'candidates.txt').write_bytes(payload)

    clean_filters = None
    if has_filters:
        clean_filters = {
            k: (list(v) if isinstance(v, (tuple, list, set)) else v)
            for k, v in (filters or {}).items()
            if v is not None
        }
    meta = {
        'species': species,
        'tier': tier,
        'tier_label': t['label'],
        'total': len(rows),
        'enumeration_mode': enumeration_mode,
        'filtered_space_size': space_size,
        'filtered_space_exact': exact_space,
        'filters': clean_filters,
        'sources': {
            'panel': len(panel),
            'forced': n_forced,
            'sampled': len(sampled) if enumeration_mode == 'sampled' else 0,
            'enumerated': len(rows) if enumeration_mode == 'exhaustive' else 0,
        },
        'sample_seed': SAMPLE_SEED,
        'panel_seed': PANEL_SEED,
        'folds': ['definition B (max<=16 skipped)', 'definition A (wasted_point)'],
        'scheduled_games': tier_games(tier) if not has_filters and max_candidates is None else None,
        'sha256': hashlib.sha256(payload).hexdigest(),
    }
    (outdir / 'candidates_meta.json').write_text(
        json.dumps(meta, indent=1, ensure_ascii=False) + '\n', encoding='utf-8'
    )
    run_cfg = {'species': species, 'tier': tier}
    if clean_filters:
        run_cfg['filters'] = clean_filters
    (outdir / 'run_config.json').write_text(
        json.dumps(run_cfg, indent=1, ensure_ascii=False) + '\n', encoding='utf-8'
    )
    print(json.dumps(meta, indent=1, ensure_ascii=False))


def print_tiers():
    for name, t in TIERS.items():
        print(f"{name:10s} ({t['label']}): candidates {t['candidates']}, "
              f"scheduled games {tier_games(name)}")


def main():
    parser = argparse.ArgumentParser(description='Generate candidates and config for species 1on1 mirror search.')
    sub = parser.add_subparsers(dest='cmd')
    gen_p = sub.add_parser('gen', help='Generate candidates and run_config.json')
    gen_p.add_argument('species', help='Base species key in data/species_dataset.json (e.g. gyarados)')
    gen_p.add_argument('tier', choices=list(TIERS.keys()), help='Search tier scale')
    gen_p.add_argument('outdir', help='Output run directory')
    gen_p.add_argument('--gcc', default='gcc', help='C compiler path (default: gcc)')
    gen_p.add_argument('--candidates', type=int, default=None, help='Override target candidate count')
    gen_p.add_argument('--allocation', action='append', type=lambda p: population3.numbers(p, 6),
                       help='Restrict to explicit point allocations H,A,B,C,D,S (repeatable)')
    gen_p.add_argument('--stat-min', type=lambda p: population3.numbers(p, 6),
                       help='Minimum points per stat H,A,B,C,D,S')
    gen_p.add_argument('--stat-max', type=lambda p: population3.numbers(p, 6),
                       help='Maximum points per stat H,A,B,C,D,S')
    gen_p.add_argument('--nature', action='append', type=lambda n: population3.numbers(n, 2),
                       help='Restrict nature up,down indices (repeatable)')
    gen_p.add_argument('--move', action='append', help='Restrict candidate move pool (repeatable)')
    gen_p.add_argument('--require-move', action='append', help='Require move in every candidate (repeatable)')
    gen_p.add_argument('--move-count', action='append', type=int, help='Allowed move-set sizes 1..4 (repeatable)')
    gen_p.add_argument('--item', action='append', help='Restrict items (repeatable)')
    gen_p.add_argument('--mega', action='append', type=int, help='Restrict mega mode 0/1/2 (repeatable)')
    gen_p.add_argument('--ability', action='append', help='Restrict abilities (repeatable)')
    gen_p.add_argument('--sex', action='append', help='Restrict sex m/f/n (repeatable)')
    sub.add_parser('list', help='List available tiers and scheduled game counts')
    args = parser.parse_args()
    if args.cmd == 'gen':
        filters = {
            'allocations': args.allocation,
            'stat_min': args.stat_min,
            'stat_max': args.stat_max,
            'natures': args.nature,
            'move_pool': args.move,
            'required_moves': args.require_move,
            'move_counts': args.move_count,
            'items': args.item,
            'megas': args.mega,
            'abilities': args.ability,
            'sexes': args.sex,
        }
        try:
            generate(args.species, args.tier, args.outdir, gcc=args.gcc,
                     filters=filters if any(v is not None for v in filters.values()) else None,
                     max_candidates=args.candidates)
        except ValueError as exc:
            parser.error(f'{exc} (use base species id from data/species_dataset.json, e.g. gyarados)')
    else:
        print(__doc__)
        print_tiers()


if __name__ == '__main__':
    main()
