"""Generate deterministic Stage 8 v3 populations for engine screen and screenx."""
import argparse
from dataclasses import dataclass
from functools import lru_cache
from itertools import combinations, permutations
import math
import os
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
NATURES = ((0, 0), *permutations(range(1, 6), 2))
MASK64 = (1 << 64) - 1


def hash64(z):
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK64
    return z ^ (z >> 31)


class Stream:
    def __init__(self, seed):
        self.state = seed

    def pick(self, n):
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK64
        return hash64(self.state) % n


@lru_cache(maxsize=1)
def move_ids():
    """The v3 parser accepts both canonical IDs and generated move codes."""
    header = (ROOT / 'engine' / 'stage8' / 'species_data.h').read_text(encoding='utf-8')
    rows = header.split('static const S8Move S8_MOVES[] = {\n', 1)[1].split('\n};', 1)[0]
    aliases = {}
    for row in rows.splitlines():
        move, code = re.match(r'\s*\{"([^"]+)", "([^"]+)",', row).groups()
        for token in (move, code):
            if aliases.setdefault(token, move) != move:
                raise ValueError(f'ambiguous move token in species_data.h: {token}')
    return aliases


def canonical_move(token):
    try:
        return move_ids()[token]
    except KeyError:
        raise ValueError(f'unknown move: {token}') from None


def numbers(field, count):
    tokens = field.split(',')
    if len(tokens) != count or any(not t.isascii() or not t.isdecimal() for t in tokens):
        raise ValueError(f'expected {count} comma-separated integers: {field}')
    return tuple(map(int, tokens))


def identity(spec):
    """Key a v3 build by its fields and move set, omitting the pilot."""
    fields = spec.strip().split('|')
    if ':' not in fields[0]:
        # Legacy rows contain final stats. Keep their tokens and separate
        # namespace since their v3 allocation/nature cannot be recovered uniquely.
        if not 3 <= len(fields) <= 7:
            raise ValueError('invalid legacy spec')
        stats, moves, _, item, mode, ability, nature = fields + [''] * (7 - len(fields))
        return 'legacy', stats, tuple(sorted(moves.split(','))), item, mode, ability, nature
    if len(fields) != 8 or not all(fields):
        raise ValueError('expected eight nonempty v3 fields')
    build, moves, _, item, mode, ability, nature, sex = fields
    species, points = build.split(':', 1)
    move_set = tuple(sorted(map(canonical_move, moves.split(','))))
    if not 1 <= len(move_set) <= 4 or len(move_set) != len(set(move_set)) or 'struggle' in move_set:
        raise ValueError('expected one to four distinct selectable moves')
    return ('v3', species, numbers(points, 6), numbers(nature, 2), sex, ability,
            item, numbers(mode, 1)[0], move_set)


@dataclass(frozen=True)
class Profile:
    sex: str
    item: str
    mega: int
    ability: str
    base_stats: tuple[tuple[int, ...], ...]


@dataclass(frozen=True)
class Domain:
    species: str
    moves_by_sex: dict[str, tuple[str, ...]]
    profiles: tuple[Profile, ...]


def build_catalog(executable, cc='gcc'):
    subprocess.run([cc, '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    str(HERE / 'population3_catalog.c'), '-lm', '-o', str(executable)],
                   check=True)


def load_domain(executable, species):
    try:
        output = subprocess.check_output(
            [str(executable), 'catalog', species],
            text=True, encoding='utf-8', stderr=subprocess.PIPE
        )
    except subprocess.CalledProcessError as exc:
        msg = (exc.stderr or '').strip()
        raise ValueError(msg or f'failed to load species catalog for "{species}"') from None
    moves_by_sex = {}
    profiles = []
    for line in output.splitlines():
        kind, sex, *values = line.split('\t')
        if kind == 'move':
            moves_by_sex.setdefault(sex, []).append(values[0])
        elif kind == 'profile':
            item, mega, ability, *bases = values
            stats = tuple(tuple(map(int, bases[i:i + 6])) for i in range(0, len(bases), 6))
            profiles.append(Profile(sex, item, int(mega), ability, stats))
        else:
            raise ValueError(f'unexpected catalog row: {line}')
    return Domain(species, {sex: tuple(sorted(set(moves))) for sex, moves in moves_by_sex.items()},
                  tuple(profiles))


def normalize_stat_bounds(stat_min=None, stat_max=None):
    lo = (0, 0, 0, 0, 0, 0) if stat_min is None else tuple(stat_min)
    hi = (32, 32, 32, 32, 32, 32) if stat_max is None else tuple(stat_max)
    if len(lo) != 6 or len(hi) != 6:
        raise ValueError('stat bounds require 6 integers')
    for i in range(6):
        if type(lo[i]) is not int or type(hi[i]) is not int or not (0 <= lo[i] <= hi[i] <= 32):
            raise ValueError(f'invalid stat bound at index {i}: {lo[i]}..{hi[i]}')
    return lo, hi


def count_allocations(stat_min=None, stat_max=None):
    """Exact count of 6-tuples in [stat_min, stat_max] summing to 66 with max > 16."""
    lo, hi = normalize_stat_bounds(stat_min, stat_max)
    if sum(lo) > 66 or sum(hi) < 66 or max(hi) <= 16:
        return 0

    def dp_count(upper_caps):
        dp = [0] * 67
        dp[0] = 1
        for a, b in zip(lo, upper_caps):
            if a > b:
                return 0
            nxt = [0] * 67
            for s in range(67):
                c = dp[s]
                if not c:
                    continue
                lim = min(b, 66 - s)
                for v in range(a, lim + 1):
                    nxt[s + v] += c
            dp = nxt
        return dp[66]

    total_all = dp_count(hi)
    total_le16 = dp_count(tuple(min(16, h) for h in hi))
    return total_all - total_le16


def allocations_for(total=66, slots=6):
    if slots == 1:
        yield (total,)
        return
    for points in range(max(0, total - 32 * (slots - 1)), min(32, total) + 1):
        for rest in allocations_for(total - points, slots - 1):
            yield (points, *rest)


def allocations_matching(stat_min=None, stat_max=None):
    """Yield 6-tuples in [stat_min, stat_max] summing to 66 with max > 16."""
    lo, hi = normalize_stat_bounds(stat_min, stat_max)
    if sum(lo) > 66 or sum(hi) < 66 or max(hi) <= 16:
        return
    suf_min = [0] * 7
    suf_max = [0] * 7
    for i in range(5, -1, -1):
        suf_min[i] = suf_min[i + 1] + lo[i]
        suf_max[i] = suf_max[i + 1] + hi[i]

    def rec(idx, rem, cur, max_so_far):
        if idx == 6:
            if rem == 0 and max_so_far > 16:
                yield tuple(cur)
            return
        start = max(lo[idx], rem - suf_max[idx + 1])
        stop = min(hi[idx], rem - suf_min[idx + 1])
        for v in range(start, stop + 1):
            cur.append(v)
            yield from rec(idx + 1, rem - v, cur, max(max_so_far, v))
            cur.pop()

    yield from rec(0, 66, [], 0)


def wasted_point(points, down, forms):
    if not down or not points[down]:
        return False
    # A: one point can move to a neutral uncapped stat without losing
    # even one actual point in any form reachable under this mega mode.
    for base in forms:
        raw = base[down] + 20 + points[down]
        if raw * 9 // 10 != (raw - 1) * 9 // 10:
            return False
    return True


def _validate_and_prepare(domain, *, allocations=None, stat_min=None, stat_max=None,
                          natures=NATURES, move_pool=None, required_moves=None,
                          items=None, megas=None, abilities=None, sexes=None,
                          move_counts=(1, 2, 3, 4)):
    if allocations is not None:
        allocations = tuple(sorted(set(map(tuple, allocations))))
        if any(len(p) != 6 or any(type(v) is not int or not 0 <= v <= 32 for v in p) or sum(p) != 66
               for p in allocations):
            raise ValueError('allocations need six integers in 0..32 totaling 66')
        allocations = tuple(p for p in allocations if max(p) > 16)
        if not allocations:
            raise ValueError('all specified allocations were discarded by Definition B (max <= 16)')
    lo, hi = normalize_stat_bounds(stat_min, stat_max)
    if allocations is not None and (stat_min is not None or stat_max is not None):
        allocations = tuple(p for p in allocations if all(lo[i] <= p[i] <= hi[i] for i in range(6)))
        if not allocations:
            raise ValueError('no specified allocations satisfy stat bounds')

    natures = tuple(sorted(set(map(tuple, natures))))
    if not natures or any(nature not in NATURES or any(type(v) is not int for v in nature) for nature in natures):
        raise ValueError('nature must be 0,0 or distinct indices 1..5')

    move_counts = tuple(sorted(set(move_counts)))
    if not move_counts or any(type(k) is not int or not 1 <= k <= 4 for k in move_counts):
        raise ValueError('move counts must be in 1..4')

    all_sex_moves = set().union(*domain.moves_by_sex.values()) if domain.moves_by_sex else set()
    if move_pool is not None:
        move_pool = set(map(canonical_move, move_pool))
        unavailable = move_pool.difference(all_sex_moves)
        if unavailable:
            raise ValueError(f'{domain.species}: moves unavailable for screening: {sorted(unavailable)}')
    if required_moves is not None:
        required_moves = tuple(sorted(set(map(canonical_move, required_moves))))
        unavailable = set(required_moves).difference(all_sex_moves)
        if unavailable:
            raise ValueError(f'{domain.species}: required moves unavailable: {sorted(unavailable)}')
        if move_pool is not None:
            outside = set(required_moves).difference(move_pool)
            if outside:
                raise ValueError(f'{domain.species}: required moves outside move pool: {sorted(outside)}')
        if len(required_moves) > max(move_counts):
            raise ValueError('more required moves than maximum move count')
    else:
        required_moves = ()

    if items is not None:
        items = set(items)
        unavailable = items.difference(p.item for p in domain.profiles)
        if unavailable:
            raise ValueError(f'{domain.species}: items unavailable for screening: {sorted(unavailable)}')
    if megas is not None:
        megas = set(megas)
        if any(type(m) is not int or m not in (0, 1, 2) for m in megas):
            raise ValueError('megas must be integers in {0, 1, 2}')
        unavailable = megas.difference(p.mega for p in domain.profiles)
        if unavailable:
            raise ValueError(f'{domain.species}: mega modes unavailable: {sorted(unavailable)}')
    if abilities is not None:
        abilities = set(abilities)
        unavailable = abilities.difference(p.ability for p in domain.profiles)
        if unavailable:
            raise ValueError(f'{domain.species}: abilities unavailable: {sorted(unavailable)}')
    if sexes is not None:
        sexes = set(sexes)
        if any(s not in ('m', 'f', 'n') for s in sexes):
            raise ValueError('sex must be in {m, f, n}')
        unavailable = sexes.difference(p.sex for p in domain.profiles)
        if unavailable:
            raise ValueError(f'{domain.species}: sexes unavailable: {sorted(unavailable)}')

    profile_entries = []
    for profile in domain.profiles:
        if items is not None and profile.item not in items:
            continue
        if megas is not None and profile.mega not in megas:
            continue
        if abilities is not None and profile.ability not in abilities:
            continue
        if sexes is not None and profile.sex not in sexes:
            continue
        sex_moves = set(domain.moves_by_sex.get(profile.sex, ()))
        if not set(required_moves).issubset(sex_moves):
            continue
        pool = tuple(m for m in domain.moves_by_sex.get(profile.sex, ())
                     if (move_pool is None or m in move_pool) and m not in required_moves)
        req = tuple(sorted(required_moves))
        sizes = tuple(k for k in move_counts if len(req) <= k <= len(req) + len(pool))
        if sizes:
            profile_entries.append((profile, req, pool, sizes))
    if not profile_entries:
        raise ValueError(f'{domain.species}: no legal builds remain under the specified filters')

    return allocations, lo, hi, natures, profile_entries


def estimate_filtered_count(domain, *, allocations=None, stat_min=None, stat_max=None,
                            natures=NATURES, move_pool=None, required_moves=None,
                            items=None, megas=None, abilities=None, sexes=None,
                            move_counts=(4,)):
    """Return exact count (when allocations <= 25000) or tight upper bound of matching builds."""
    allocations, lo, hi, natures, profile_entries = _validate_and_prepare(
        domain, allocations=allocations, stat_min=stat_min, stat_max=stat_max,
        natures=natures, move_pool=move_pool, required_moves=required_moves,
        items=items, megas=megas, abilities=abilities, sexes=sexes, move_counts=move_counts,
    )
    n_alloc = len(allocations) if allocations is not None else count_allocations(lo, hi)
    if n_alloc == 0:
        return 0, True

    move_combos_by_profile = [
        sum(math.comb(len(pool), size - len(req)) for size in sizes)
        for _, req, pool, sizes in profile_entries
    ]
    if n_alloc <= 25000:
        alloc_list = allocations if allocations is not None else tuple(allocations_matching(lo, hi))
        exact = 0
        for (profile, _, _, _), n_moves in zip(profile_entries, move_combos_by_profile):
            valid_an = sum(
                1
                for pts in alloc_list
                for _, down in natures
                if not wasted_point(pts, down, profile.base_stats)
            )
            exact += valid_an * n_moves
        return exact, True

    upper = sum(
        len(natures) * n_alloc * n_moves
        for n_moves in move_combos_by_profile
    )
    return upper, False


def candidates(domain, *, allocations=None, stat_min=None, stat_max=None,
               natures=NATURES, move_pool=None, required_moves=None,
               items=None, megas=None, abilities=None, sexes=None,
               move_counts=(1, 2, 3, 4)):
    """Yield canonical obsh4 specs. Optional bounds select an explicit subdomain."""
    allocations, lo, hi, natures, profile_entries = _validate_and_prepare(
        domain, allocations=allocations, stat_min=stat_min, stat_max=stat_max,
        natures=natures, move_pool=move_pool, required_moves=required_moves,
        items=items, megas=megas, abilities=abilities, sexes=sexes, move_counts=move_counts,
    )
    if allocations is None:
        if lo == (0,) * 6 and hi == (32,) * 6:
            profile_allocations = tuple(p for p in allocations_for() if max(p) > 16)
        else:
            profile_allocations = tuple(allocations_matching(lo, hi))
    else:
        profile_allocations = allocations

    for profile, req, opt_pool, sizes in profile_entries:
        for points in profile_allocations:
            point_field = ','.join(map(str, points))
            for up, down in natures:
                if wasted_point(points, down, profile.base_stats):
                    continue
                suffix = f'|obsh4|{profile.item}|{profile.mega}|{profile.ability}|{up},{down}|{profile.sex}'
                for size in sizes:
                    need = size - len(req)
                    for rest in combinations(opt_pool, need):
                        moves = ','.join(sorted((*req, *rest)))
                        yield f'{domain.species}:{point_field}|{moves}{suffix}'


def sample_filtered_builds(domain, seed, count, seen, *, allocations=None,
                           stat_min=None, stat_max=None, natures=NATURES,
                           move_pool=None, required_moves=None, items=None,
                           megas=None, abilities=None, sexes=None,
                           move_counts=(4,)):
    """Deterministically sample up to `count` unique specs matching explicit filters."""
    allocations, lo, hi, natures, profile_entries = _validate_and_prepare(
        domain, allocations=allocations, stat_min=stat_min, stat_max=stat_max,
        natures=natures, move_pool=move_pool, required_moves=required_moves,
        items=items, megas=megas, abilities=abilities, sexes=sexes, move_counts=move_counts,
    )
    if allocations is None:
        n_alloc = count_allocations(lo, hi)
        if n_alloc == 0:
            return []
        alloc_pool = tuple(allocations_matching(lo, hi)) if n_alloc <= 25000 else None
    else:
        alloc_pool = allocations

    rng = Stream(seed)
    out = []
    i = 0
    stalls = 0
    max_stalls = max(20000, count * 20)
    span = [hi[k] - lo[k] + 1 for k in range(6)]
    while len(out) < count and stalls < max_stalls:
        profile, req, opt_pool, sizes = profile_entries[i % len(profile_entries)]
        up, down = natures[(i // len(profile_entries)) % len(natures)]
        i += 1
        if alloc_pool is not None:
            pts = alloc_pool[rng.pick(len(alloc_pool))]
            if wasted_point(pts, down, profile.base_stats):
                stalls += 1
                continue
        else:
            found = None
            for _ in range(1024):
                trial = [lo[k] + rng.pick(span[k]) for k in range(6)]
                if sum(trial) != 66 or max(trial) <= 16:
                    continue
                if wasted_point(trial, down, profile.base_stats):
                    continue
                found = tuple(trial)
                break
            if found is None:
                stalls += 1
                continue
            pts = found

        size = sizes[rng.pick(len(sizes))] if len(sizes) > 1 else sizes[0]
        need = size - len(req)
        pool = list(opt_pool)
        chosen = list(req)
        for _ in range(need):
            chosen.append(pool.pop(rng.pick(len(pool))))
        spec = (f"{domain.species}:{','.join(map(str, pts))}|{','.join(sorted(chosen))}"
                f"|obsh4|{profile.item}|{profile.mega}|{profile.ability}|{up},{down}|{profile.sex}")
        ident = identity(spec)
        if ident in seen:
            stalls += 1
            continue
        seen.add(ident)
        out.append(spec)
        stalls = 0
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('species', help='canonical normal-form species key')
    parser.add_argument('--allocation', action='append', type=lambda p: numbers(p, 6),
                        help='restrict to these six comma-separated points (repeatable)')
    parser.add_argument('--stat-min', type=lambda p: numbers(p, 6),
                        help='minimum points per stat (H,A,B,C,D,S)')
    parser.add_argument('--stat-max', type=lambda p: numbers(p, 6),
                        help='maximum points per stat (H,A,B,C,D,S)')
    parser.add_argument('--nature', action='append', type=lambda n: numbers(n, 2),
                        help='restrict to these comma-separated boosted/hindered indices (repeatable)')
    parser.add_argument('--move', action='append', help='restrict the learnset to this ID or code (repeatable)')
    parser.add_argument('--require-move', action='append', help='require this move in every build (repeatable)')
    parser.add_argument('--item', action='append', help='restrict items to this canonical ID (repeatable)')
    parser.add_argument('--mega', action='append', type=int, help='restrict mega mode 0/1/2 (repeatable)')
    parser.add_argument('--ability', action='append', help='restrict abilities to this canonical ID (repeatable)')
    parser.add_argument('--sex', action='append', help='restrict sex m/f/n (repeatable)')
    parser.add_argument('--move-count', action='append', type=int, help='restrict move-set size (repeatable)')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='population3-') as tmp:
        catalog = Path(tmp) / ('catalog.exe' if os.name == 'nt' else 'catalog')
        try:
            build_catalog(catalog)
            domain = load_domain(catalog, args.species)
            for spec in candidates(
                domain,
                allocations=args.allocation,
                stat_min=args.stat_min,
                stat_max=args.stat_max,
                natures=args.nature or NATURES,
                move_pool=args.move,
                required_moves=args.require_move,
                items=args.item,
                megas=args.mega,
                abilities=args.ability,
                sexes=args.sex,
                move_counts=args.move_count or (1, 2, 3, 4),
            ):
                print(spec)
        except (ValueError, subprocess.CalledProcessError) as error:
            parser.error(str(error))


if __name__ == '__main__':
    main()
