"""Build the stage-4 held-out population from run1/candidates2.txt.
Mirrors prepare.py's adaptation; excludes every training-population build;
takes an evenly spaced deterministic sample of 176 (168 + 8)."""
import hashlib, json
from pathlib import Path
root = Path(__file__).resolve().parent.parent
s3 = root / 'stage3'

def adapt(line):
    fields = line.split('|')
    if len(fields[1].split(',')) == 3:
        fields[1] += ',ST'
    fields[2] = 'generic'
    return '|'.join(fields)

training = set((s3 / 'population.txt').read_text().splitlines())
src = (root / 'run1/candidates2.txt').read_text().splitlines()
seen = set()
pool = []  # (adapted_spec, source_line_number)
for i, line in enumerate(src):
    a = adapt(line)
    if a in training or a in seen:
        continue
    seen.add(a)
    pool.append((a, i + 1))
print(f'source {len(src)} -> unique non-training {len(pool)}')
step = len(pool) // 176
picked = [pool[i * step] for i in range(176)]
assert len({p[0] for p in picked}) == 176
overlap = sum(1 for p in picked if p[0] in training)
print(f'step {step}; picked 176; overlap with training population: {overlap}')
assert overlap == 0
out = s3 / 'heldout_population.txt'
out.write_text('\n'.join(p[0] for p in picked) + '\n')
(s3 / 'heldout_population.json').write_text(json.dumps(
    [dict(id=i, source='run1/candidates2.txt', line=p[1], regular_spec=p[0]) for i, p in enumerate(picked)], indent=2) + '\n')
(s3 / 'heldout_population.sha256').write_text(hashlib.sha256(out.read_bytes()).hexdigest() + '  heldout_population.txt\n')
print('wrote', out)
