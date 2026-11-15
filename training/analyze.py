"""Validate evaluation bytes and report paired-seat, seed-block 95% CIs."""
import argparse,csv,json
from pathlib import Path
import numpy as np
p=argparse.ArgumentParser(); p.add_argument('prefix'); args=p.parse_args()
path=Path(args.prefix); raw=np.frombuffer(Path(str(path)+'.bin').read_bytes(),np.uint8)
rows=list(csv.DictReader(open(str(path)+'.csv'))); results=[]; grouped={}
rng=np.random.default_rng(73104)
for row in rows:
    n=int(row['n']); off=int(row['offset']); r=raw[off:off+2*n].reshape(n,2); score=r[:,0]/2
    assert np.isin(r[:,0],[0,1,2]).all() and ((r[:,1]>=1)&(r[:,1]<=100)).all()
    for key,value in [('w',np.sum(r[:,0]==2)),('l',np.sum(r[:,0]==0)),('d',np.sum(r[:,0]==1)),('turns',r[:,1].sum()),('cap',np.sum(r[:,1]==100))]: assert int(row[key])==value,(key,row)
    assert int(row['fallbacks'])==0
    grouped.setdefault((row['population'],row['opponent']),[]).append((int(row['seat']),score,r[:,1],row))
assert len(raw)==sum(2*int(r['n']) for r in rows)
def ci(x):
    # Resample whole seed/build blocks, pairing both seats within each block.
    samples=np.concatenate([x[rng.integers(0,len(x),(100,len(x)))].mean(1) for _ in range(20)])
    lo,hi=np.quantile(samples,[.025,.975])
    if np.ptp(x)==0:
        eps=np.sqrt(np.log(40)/(2*len(x))); lo=max(0,x.mean()-eps); hi=min(1,x.mean()+eps)
    return [float(lo*100),float(hi*100)]
for (pop,opp),group in grouped.items():
    group.sort(key=lambda x:x[0]); assert [g[0] for g in group]==[0,1]
    scores=(group[0][1]+group[1][1])/2
    result=dict(population=pop,opponent=opp,n_per_seat=len(scores),score=float(scores.mean()*100),ci95=ci(scores),seats=[])
    for seat,score,turns,row in group:
        result['seats'].append(dict(seat=seat,score=float(score.mean()*100),ci95=ci(score),w=int(row['w']),l=int(row['l']),d=int(row['d']),mean_turns=float(turns.mean()),cap=int(row['cap']),fallbacks=0))
    results.append(result)
Path(str(path)+'.json').write_text(json.dumps(results,indent=2)+'\n')
lines=['| Population | Opponent | Learner seat A score % (95% CI) | Learner seat B | Both seats |','|---|---|---:|---:|---:|']
def cell(s): return f"{s['score']:.2f} [{s['ci95'][0]:.2f}, {s['ci95'][1]:.2f}]"
for r in results: lines.append(f"| {r['population']} | {r['opponent']} | {cell(r['seats'][0])} | {cell(r['seats'][1])} | {cell(r)} |")
Path(str(path)+'.md').write_text('\n'.join(lines)+'\n'); print('\n'.join(lines))
