"""Hash all reviewable source/data/results; no environment secrets."""
from pathlib import Path
import hashlib,json,platform,sys,subprocess
S=Path(__file__).resolve().parent; root=S.parent
files=[root/'engine.c',root/'engine',root/'observation_test.c',root/'README.md']
files+=sorted(p for p in S.rglob('*') if p.is_file() and p.name not in ('artifacts.sha256','artifact_integrity.txt') and '__pycache__' not in p.parts)
(S/'artifacts.sha256').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+str(p.relative_to(root))+'\n' for p in files))
