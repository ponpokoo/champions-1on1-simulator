"""Generate deterministic Stage 8 v3 populations for engine screen and screenx."""
import argparse
from dataclasses import dataclass
from functools import lru_cache
from itertools import combinations, permutations
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
NATURES = ((0, 0), *permutations(range(1, 6), 2))


@lru_cache(maxsize=1)
def move_ids():
    """The v3 parser accepts both canonical IDs and generated move codes."""
    header = (HERE / 'species_data.h').read_text(encoding='utf-8')
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
    output = subprocess.check_output([executable, 'catalog', species], text=True)
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


def allocations_for(total=66, slots=6):
    if slots == 1:
        yield (total,)
        return
    for points in range(max(0, total - 32 * (slots - 1)), min(32, total) + 1):
        for rest in allocations_for(total - points, slots - 1):
            yield (points, *rest)


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


def candidates(domain, *, allocations=None, natures=NATURES, move_pool=None, items=None,
               move_counts=(1, 2, 3, 4)):
    """Yield canonical obsh4 specs. Optional bounds select an explicit subdomain."""
    if allocations is not None:
        allocations = tuple(sorted(set(map(tuple, allocations))))
        if any(len(p) != 6 or any(type(v) is not int or not 0 <= v <= 32 for v in p) or sum(p) != 66
               for p in allocations):
            raise ValueError('allocations need six integers in 0..32 totaling 66')
    natures = tuple(sorted(set(map(tuple, natures))))
    if any(nature not in NATURES or any(type(v) is not int for v in nature) for nature in natures):
        raise ValueError('nature must be 0,0 or distinct indices 1..5')
    move_counts = tuple(sorted(set(move_counts)))
    if not move_counts or any(type(k) is not int or not 1 <= k <= 4 for k in move_counts):
        raise ValueError('move counts must be in 1..4')
    if move_pool is not None:
        move_pool = set(map(canonical_move, move_pool))
        unavailable = move_pool.difference(*domain.moves_by_sex.values())
        if unavailable:
            raise ValueError(f'{domain.species}: moves unavailable for screening: {sorted(unavailable)}')
    if items is not None:
        items = set(items)
        unavailable = items.difference(p.item for p in domain.profiles)
        if unavailable:
            raise ValueError(f'{domain.species}: items unavailable for screening: {sorted(unavailable)}')

    for profile in domain.profiles:
        if items is not None and profile.item not in items:
            continue
        pool = tuple(m for m in domain.moves_by_sex.get(profile.sex, ()) if move_pool is None or m in move_pool)
        sizes = tuple(k for k in move_counts if k <= len(pool))
        if not sizes:
            continue
        profile_allocations = allocations_for() if allocations is None else allocations
        for points in profile_allocations:
            if max(points) <= 16:  # B: the approved uniform allocation cutoff.
                continue
            point_field = ','.join(map(str, points))
            for up, down in natures:
                if wasted_point(points, down, profile.base_stats):
                    continue
                suffix = f'|obsh4|{profile.item}|{profile.mega}|{profile.ability}|{up},{down}|{profile.sex}'
                for size in sizes:
                    for moves in combinations(pool, size):
                        yield f'{domain.species}:{point_field}|{",".join(moves)}{suffix}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('species', help='canonical normal-form species key')
    parser.add_argument('--allocation', action='append', type=lambda p: numbers(p, 6),
                        help='restrict to these six comma-separated points (repeatable)')
    parser.add_argument('--nature', action='append', type=lambda n: numbers(n, 2),
                        help='restrict to these comma-separated boosted/hindered indices (repeatable)')
    parser.add_argument('--move', action='append', help='restrict the learnset to this ID or code (repeatable)')
    parser.add_argument('--item', action='append', help='restrict items to this canonical ID (repeatable)')
    parser.add_argument('--move-count', action='append', type=int, help='restrict move-set size (repeatable)')
    args = parser.parse_args()
    with TemporaryDirectory(prefix='population3-') as tmp:
        catalog = Path(tmp) / 'catalog'
        try:
            build_catalog(catalog)
            domain = load_domain(catalog, args.species)
            for spec in candidates(domain, allocations=args.allocation, natures=args.nature or NATURES,
                                   move_pool=args.move, items=args.item, move_counts=args.move_count or (1, 2, 3, 4)):
                print(spec)
        except (ValueError, subprocess.CalledProcessError) as error:
            parser.error(str(error))


if __name__ == '__main__':
    main()
