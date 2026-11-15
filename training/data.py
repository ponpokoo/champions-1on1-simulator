"""MGT3 decision-only data. Metadata is never passed to encode() or either head."""
import gzip,struct
from pathlib import Path
import numpy as np
DTYPE=np.dtype([('obs','<i2',(181,)),('lock','<u4'),('mask','u1'),('action','u1'),('score','u1'),('seat','u1'),('opponent','u1'),('turns','u1'),('self_id','<u2'),('foe_id','<u2'),('game','<u4'),('logp','<f4'),('value','<f4'),('prior','u1'),('reserved','u1')])
assert DTYPE.itemsize==390

def validation_mask(game_ids, seed):
    # Full mixing avoids correlating g%4 opponent groups with a modulo split.
    z=np.asarray(game_ids,dtype=np.uint64)+np.uint64(seed)
    z=(z^(z>>30))*np.uint64(0xBF58476D1CE4E5B9)
    z=(z^(z>>27))*np.uint64(0x94D049BB133111EB)
    return ((z^(z>>31))%10)==0

def read(path, expected_hash=None):
    raw=gzip.open(path,'rb').read() if str(path).endswith('.gz') else Path(path).read_bytes()
    magic,version,words,active,size,games,seed,digest=struct.unpack('<4sIIIIIQ32s',raw[:64])
    if (magic,version,words,active,size)!=(b'MGT3',1,256,181,390): raise ValueError('MGT3 header')
    if expected_hash and digest.hex()!=expected_hash: raise ValueError('behavior model hash mismatch')
    a=np.frombuffer(raw[64:],dtype=DTYPE)
    o=np.zeros((len(a),256),np.int32); o[:,:181]=a['obs']; o[:,117]=a['lock']
    masks=((a['mask'][:,None]>>np.arange(5))&1).astype(bool)
    if not len(a) or np.any(a['action']>=5) or np.any(a['prior']>=5): raise ValueError('action range')
    if not masks[np.arange(len(a)),a['action']].all() or not masks[np.arange(len(a)),a['prior']].all(): raise ValueError('invalid chosen/prior action')
    if not np.array_equal(masks,o[:,176:181]) or not np.all(o[:,1]==1) or np.any(o[:,2]): raise ValueError('mask/profile/terminal')
    if np.any(a['obs'][:,117]) or np.any(a['reserved']) or np.any(a['score']>2) or np.any(a['seat']>1): raise ValueError('reserved/range')
    ids,counts=np.unique(a['game'],return_counts=True)
    if not np.array_equal(ids,np.arange(games)): raise ValueError('missing game')
    starts=np.r_[0,np.cumsum(counts)[:-1]]
    if not np.array_equal(np.repeat(ids,counts),a['game']): raise ValueError('game grouping')
    for key in ['score','seat','opponent','turns','self_id','foe_id']:
        if not np.array_equal(a[key],np.repeat(a[key][starts],counts)): raise ValueError('inconsistent game '+key)
    if not np.array_equal(counts,a['turns'][starts]): raise ValueError('missing decision')
    if not np.array_equal(o[:,0],np.concatenate([np.arange(1,c+1) for c in counts])): raise ValueError('decision timing')
    if not np.isfinite(a['logp']).all() or not np.isfinite(a['value']).all(): raise ValueError('nonfinite behavior')
    return a,o,masks,dict(games=games,seed=seed,model_hash=digest.hex(),decisions=len(a),bytes=len(raw))
