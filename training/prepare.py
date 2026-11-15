"""Read-only run1 inputs; profile adaptation is explicit and isolated."""
from pathlib import Path
import hashlib,json
root=Path(__file__).resolve().parent.parent
finals=(root/'run1/finalists.txt').read_text().splitlines()
bench=(root/'run1/pool.txt').read_text().splitlines()
assert len(finals)==168 and len(bench)==8
adapted=[]; provenance=[]
for i,line in enumerate(finals+bench):
    fields=line.split('|'); before=line
    if len(fields[1].split(','))==3: fields[1]+=',ST'
    fields[2]='generic'
    adapted.append('|'.join(fields))
    provenance.append(dict(id=i,source='run1/finalists.txt' if i<168 else 'run1/pool.txt',line=i+1 if i<168 else i-167,original=before,regular_spec=adapted[-1]))
out=root/'stage3/population.txt'; out.write_text('\n'.join(adapted)+'\n')
(root/'stage3/population.json').write_text(json.dumps(provenance,indent=2)+'\n')
(root/'stage3/population.sha256').write_text(hashlib.sha256(out.read_bytes()).hexdigest()+'  population.txt\n')
