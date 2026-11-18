"""Deterministic full-learnset populations; never reads training outcomes."""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MASK64 = (1 << 64) - 1
ITEMS = ('none leftovers sitrus scarf lifeorb muscle wise belt tag sand spoon '
         'bbelt metronome shellbell lens kings wide zoom powder claw ironball '
         'shuca chople kasib payapa chesto lum leppa oran sash fband balloon herb helmet').split()
CORES = ((155, 155, 150, 115, 110, 90), (155, 165, 170, 125, 130, 130))
NATURES = [(u, d) for u in range(1, 6) for d in range(1, 6) if u != d] + [(0, 0)]
SELECTION_SEED = 20261803
CURRICULUM_SEED = 20261804
CURRICULUM_CODES = ('BP BD EQ SCL SB MS AGI CP ID PRO PU REST SF ST SUB TRK PN '
                    'SP BUL BSW IH MM TB IP BSL REF LS BB MR TAN').split()
CURRICULUM_FILE = 'population_campaign5.txt'
FROZEN_FILES = ('population.txt', 'heldout.txt', 'selection.txt', 'panel.txt')


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


def identity(spec):
    """Policy and move-slot permutations cannot disguise a held-out overlap."""
    s = spec.split('|')
    return s[0], tuple(sorted(s[1].split(','))), s[3], s[4], s[5] if s[4] == '0' else 'tc'


def move_codes():
    rows = (ROOT / 'engine.c').read_text(encoding='utf-8').split('static const MoveDef MOVES[MV_COUNT] = {', 1)[1].split('};', 1)[0]
    codes = re.findall(r'\{"(\w+)"', rows)
    assert len(codes) == 68
    return codes


def validate(directory):
    directory = Path(directory)
    files = ['population.txt', 'heldout.txt', 'panel.txt', 'selection.txt']
    pools = [(directory / f).read_text(encoding='utf-8').splitlines() for f in files]
    keys = [set(map(identity, p)) for p in pools]
    for p, k in zip(pools, keys):
        if len(p) != len(k) or not p:
            raise ValueError('empty/duplicate population')
    overlaps = {f'{files[i]}:{files[j]}': len(keys[i] & keys[j])
                for i in range(len(files)) for j in range(i + 1, len(files))}
    if any(overlaps.values()):
        raise ValueError('population overlap: ' + str(overlaps))
    codes = move_codes()
    for pool in pools:
        assert set(codes) == {m for s in pool for m in s.split('|')[1].split(',')}
        for spec in pool:
            f = spec.split('|')
            ms = f[1].split(',')
            assert len(ms) == len(set(ms)) == 4
            assert any(codes.index(m) >= 17 for m in ms), 'v2 vocabulary guard'
            assert f[2] == 'generic' and (f[4] == '1' or f[5] in ('cb', 'lm'))
    manifest = json.loads((directory / 'population.json').read_text(encoding='utf-8'))
    selection = json.loads((directory / 'selection.json').read_text(encoding='utf-8'))
    hashes = dict(manifest['sha256'], **selection['sha256'])
    assert len(pools[-1]) == 256 and selection['seed'] == SELECTION_SEED
    assert selection['excluded_sha256'] == manifest['sha256']
    for name in files:
        assert hashlib.sha256((directory / name).read_bytes()).hexdigest() == hashes[name]
    return dict(counts=dict(zip(files, map(len, pools))), overlap=overlaps,
                move_coverage=[68] * len(files), sha256=hashes, selection_seed=selection['seed'])


def generate_pool(name, n, seed, seen, vocabulary=None, mixture=None):
    """Sample global wire IDs; mixture entries are (label, count, vocabulary).

    Only accepted rows advance the global schedule and component anchor.
    Omitting both options retains the historical RNG stream and provenance.
    """
    rng = Stream(seed)
    codes = move_codes()
    if mixture is not None and vocabulary is not None:
        raise ValueError('choose vocabulary or mixture')
    components = mixture if mixture is not None else [(None, n, vocabulary if vocabulary is not None else range(68))]
    components = [(label, count, tuple(vocab)) for label, count, vocab in components]
    if not components or sum(count for _, count, _ in components) != n:
        raise ValueError('mixture quotas must sum to count')
    for _, count, vocab in components:
        if count < 1 or len(vocab) < 4 or len(set(vocab)) != len(vocab) or any(
                type(m) is not int or not 0 <= m < 68 for m in vocab) or not any(m >= 17 for m in vocab):
            raise ValueError('invalid move vocabulary/quota')
    rows, provenance = [], []
    component, component_row = 0, 0
    while len(rows) < n:
        i = len(rows)
        label, quota, vocab = components[component]
        mega = i % 2
        # Uniform bounded integer tuples conditioned on exactly 66 points.
        while True:
            alloc = [rng.pick(33) for _ in range(6)]
            if sum(alloc) == 66:
                break
        up, down = NATURES[i % 21]
        stats = [c + p for c, p in zip(CORES[mega], alloc)]
        if up:
            stats[up] = stats[up] * 110 // 100
            stats[down] = stats[down] * 90 // 100
        # Each component cycles its vocabulary using its accepted-row counter.
        moves = [vocab[component_row % len(vocab)]]
        while len(moves) < 4:
            m = vocab[rng.pick(len(vocab))]
            if m not in moves:
                moves.append(m)
        if max(moves) < 17:
            continue
        # Fisher-Yates avoids a special first-move slot distribution.
        for j in range(3, 0, -1):
            k = rng.pick(j + 1)
            moves[j], moves[k] = moves[k], moves[j]
        item = 'stone' if mega else ITEMS[(i // 2) % len(ITEMS)]
        ability = 'cb' if mega or (i // 2) % 2 == 0 else 'lm'
        spec = f"{','.join(map(str, stats))}|{','.join(codes[m] for m in moves)}|generic|{item}|{mega}|{ability}"
        if identity(spec) in seen:
            continue
        seen.add(identity(spec))
        rows.append(spec)
        entry = dict(pool=name, id=i, alloc=alloc, nature=[up, down], spec=spec)
        if mixture is not None:
            entry.update(component=label, component_row=component_row)
        provenance.append(entry)
        component_row += 1
        if component_row == quota:
            component += 1
            component_row = 0
    return rows, provenance


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def curriculum_mixture():
    codes = move_codes()
    return [('concentrated', 768, [codes.index(c) for c in CURRICULUM_CODES]),
            ('full68', 256, list(range(68)))]


def curriculum_freeze(directory):
    """Compute the freeze without touching any files or historical manifests."""
    directory = Path(directory)
    historical = validate(directory)
    seen = {identity(s) for name in ('heldout.txt', 'selection.txt', 'panel.txt')
            for s in (directory/name).read_text(encoding='utf-8').splitlines()}
    mixture = curriculum_mixture()
    rows, builds = generate_pool(CURRICULUM_FILE, 1024, CURRICULUM_SEED, seen, mixture=mixture)
    codes = move_codes()
    # The dataset's English/Japanese names are metadata, never new move IDs.
    dataset = json.loads((ROOT/'species-data/species_dataset.json').read_text(encoding='utf-8'))
    names = {re.sub('[^a-z0-9]', '', m['nameEn'].lower()): m for m in dataset['moves'].values()}
    engine_rows = (ROOT/'engine.c').read_text(encoding='utf-8').split(
        'static const MoveDef MOVES[MV_COUNT] = {', 1)[1].split('};', 1)[0]
    english = dict(re.findall(r'\{"(\w+)"[^\n]*?// ([A-Za-z][A-Za-z -]*?)(?: \(|\n|$)', engine_rows))
    # Original 17 have Japanese comments; use their established English names.
    english.update(zip(codes[:17], ('Body Press', 'Bulldoze', 'Earthquake', 'Shadow Claw', 'Shadow Ball',
        'Mud-Slap', 'Agility', 'Cosmic Power', 'Iron Defense', 'Protect', 'Psych Up', 'Rest',
        'Scary Face', 'Sleep Talk', 'Substitute', 'Trick', 'Psychic Noise')))
    # Explicit dataset keys avoid depending on explanatory engine comment suffixes.
    added_names = ('Shadow Punch', 'Bullet Punch', 'Brutal Swing', 'Iron Head', 'Meteor Mash',
                   'Trailblaze', 'Ice Punch', 'Body Slam', 'Reflect', 'Light Screen', 'Brick Break',
                   'Magnet Rise', 'Stomping Tantrum')
    english.update(zip(CURRICULUM_CODES[17:], added_names))
    vocabularies = {}
    for label, count, vocab in mixture:
        entries = []
        for wire_id in vocab:
            code = codes[wire_id]
            # Non-core English names come directly from the engine comments.
            name = english[code].strip()
            move = names[re.sub('[^a-z0-9]', '', name.lower())]
            entries.append(dict(wire_id=wire_id, code=code, name=move['nameEn'], name_jp=move['nameJp']))
        vocabularies[label] = dict(moves=entries, sha256=hashlib.sha256(
            json.dumps(entries, sort_keys=True, ensure_ascii=False).encode('utf-8')).hexdigest())
    keys = set(map(identity, rows))
    overlap = {name: len(keys & {identity(s) for s in (directory/name).read_text(encoding='utf-8').splitlines()})
               for name in FROZEN_FILES}
    if any(overlap.values()):
        raise ValueError('curriculum overlap: '+str(overlap))
    text = '\n'.join(rows)+'\n'
    manifest = dict(version=1, seed=CURRICULUM_SEED, count=1024,
        generator='population.py:generate_pool',
        source_sha256={p: sha256(ROOT/p) for p in ('campaign3/population.py', 'engine.c',
            'species-data/species_dataset.json', 'stage6/design_campaign5.md')},
        vocabularies=vocabularies,
        mixture=[dict(component=label, count=count, ratio=count/1024, first_row=start, last_row=start+count-1)
                 for (label, count, _), start in zip(mixture, (0, 768))],
        scheduling='global accepted i: form=i%2; nature=i%21; regular item=ITEMS[(i//2)%len(ITEMS)]; CB/LM=(i//2)%2',
        sampling='component accepted k anchors vocabulary[k%size]; other 3 distinct uniform draws; Fisher-Yates slots; reject if all global IDs <17; rejected attempts advance neither counter',
        domain='exactly 66 points, 0..32/stat; 21 natures; normal CB/LM or Mega TC; no synergy, attack, item or nature filters',
        exclusion='canonical stats/unordered moves/item/form/effective ability; policy excluded; Mega ability=tc',
        excluded_sha256={f: historical['sha256'][f] for f in ('heldout.txt', 'selection.txt', 'panel.txt')},
        sha256={CURRICULUM_FILE: hashlib.sha256(text.encode('utf-8')).hexdigest()},
        coverage=dict(total_moves=68, forms={'regular': 512, 'mega': 512}, regular_abilities={'cb': 256, 'lm': 256},
            components={label: dict(count=count, move_coverage=len(vocab), anchor_coverage=len(vocab),
                                   regular=count//2, mega=count//2) for label, count, vocab in mixture},
            statement='Pilot trained under this 75% concentrated/25% full-68 curriculum; all 68 global move IDs remain covered. Stage 6e build search retains all 68 moves with no role-based cuts. Finite frozen pools cannot guarantee performance against every 6e combination.'),
        overlap=overlap, builds=builds)
    return text, manifest


def validate_curriculum(directory, population=None):
    """Validate historical gates plus rows and provenance by deterministic replay."""
    directory = Path(directory)
    path = Path(population) if population is not None else directory/CURRICULUM_FILE
    rows = path.read_text(encoding='utf-8').splitlines()
    try:
        keys = set(map(identity, rows))
    except IndexError as exc:
        raise ValueError('invalid curriculum build') from exc
    if len(rows) != 1024 or len(keys) != 1024:
        raise ValueError('curriculum count/duplicate population')
    overlaps = {name: len(keys & {identity(s) for s in (directory/name).read_text(encoding='utf-8').splitlines()})
                for name in FROZEN_FILES}
    if any(overlaps.values()):
        raise ValueError('curriculum overlap: '+str(overlaps))
    expected_text, expected = curriculum_freeze(directory)
    manifest = json.loads(path.with_suffix('.json').read_text(encoding='utf-8'))
    if path.read_bytes() != expected_text.encode('utf-8'):
        raise ValueError('curriculum rows/vocabulary/coverage/schedule differ from deterministic freeze')
    if manifest != expected:
        raise ValueError('curriculum manifest/provenance/hash differs from deterministic freeze')
    return {k: manifest[k] for k in ('count', 'seed', 'sha256', 'excluded_sha256', 'mixture', 'coverage', 'overlap')}


def prepare_curriculum(directory):
    """Freeze only the new two artifacts; refuse to replace a differing freeze."""
    directory = Path(directory)
    text, manifest = curriculum_freeze(directory)
    outputs = {directory/CURRICULUM_FILE: text.encode('utf-8'),
               directory/Path(CURRICULUM_FILE).with_suffix('.json'): (json.dumps(manifest, indent=2)+'\n').encode('utf-8')}
    for path, data in outputs.items():
        if path.exists() and path.read_bytes() != data:
            raise ValueError('refusing to overwrite differing freeze: '+str(path))
    for path, data in outputs.items():
        if not path.exists():
            path.write_bytes(data)
    return validate_curriculum(directory)


def prepare_selection(directory):
    """Add selection only; never rewrite the three frozen campaign-3 pools."""
    directory = Path(directory)
    excluded = ['population.txt', 'heldout.txt', 'panel.txt']
    seen = {identity(s) for name in excluded for s in (directory/name).read_text(encoding='utf-8').splitlines()}
    rows, provenance = generate_pool('selection.txt', 256, SELECTION_SEED, seen)
    (directory/'selection.txt').write_text('\n'.join(rows)+'\n', encoding='utf-8')
    manifest = dict(version=1, seed=SELECTION_SEED, count=256,
        generator='population.py:generate_pool; identical to population/heldout rules',
        exclusion='canonical stats/unordered moves/item/form/ability; policy excluded',
        excluded_sha256={name: hashlib.sha256((directory/name).read_bytes()).hexdigest() for name in excluded},
        sha256={'selection.txt': hashlib.sha256((directory/'selection.txt').read_bytes()).hexdigest()},
        builds=provenance)
    (directory/'selection.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return validate(directory)


def prepare(directory, count=1024, heldout_count=256):
    if count < 68 or heldout_count < 68:
        raise ValueError('at least 68 builds per generated pool')
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    baseline = json.loads((ROOT / 'stage6/v2_baseline.json').read_text(encoding='utf-8'))
    panel = [b['spec'] for b in baseline['panel']]
    # Canonicalize explicit normal abilities for comparison and C parsing.
    panel = [s if len(s.split('|')) == 6 else s + '|cb' for s in panel]
    seen = set(map(identity, panel))
    provenance = []
    for name, n, seed in [('population.txt', count, 20261801), ('heldout.txt', heldout_count, 20261802)]:
        rows, builds = generate_pool(name, n, seed, seen)
        provenance.extend(builds)
        (directory / name).write_text('\n'.join(rows) + '\n', encoding='utf-8')
    (directory / 'panel.txt').write_text('\n'.join(panel) + '\n', encoding='utf-8')
    manifest = dict(version=1, seed=[20261801, 20261802],
                    domain='exactly 66 points, 0..32/stat; 21 natures; normal CB/LM or Mega TC',
                    sampling='uniform conditioned allocation; cycling form/nature/item/coverage; unpruned 4 distinct moves',
                    heldout_exclusion='canonical stats/unordered moves/item/form/ability; policy excluded',
                    panel_source='stage6/v2_baseline.json',
                    sha256={f: hashlib.sha256((directory / f).read_bytes()).hexdigest()
                            for f in ('population.txt', 'heldout.txt', 'panel.txt')},
                    builds=provenance, panel=baseline['panel'])
    (directory / 'population.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return prepare_selection(directory)


if __name__ == '__main__':
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent)
    p.add_argument('--count', type=int, default=1024)
    p.add_argument('--heldout-count', type=int, default=256)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--selection-only', action='store_true', help='freeze selection without rewriting campaign-3 pools')
    mode.add_argument('--curriculum-only', action='store_true', help='freeze campaign-5 curriculum without rewriting historical pools')
    a = p.parse_args()
    print(json.dumps(prepare_curriculum(a.out) if a.curriculum_only else
        prepare_selection(a.out) if a.selection_only else prepare(a.out, a.count, a.heldout_count), indent=2))
