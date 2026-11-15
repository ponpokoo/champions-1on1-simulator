"""Exact encoder + numerical C/PyTorch or NumPy inference comparison."""
import argparse,struct,subprocess,tempfile,json
from pathlib import Path
import numpy as np
from model import encode,load_model
p=argparse.ArgumentParser(); p.add_argument('model'); p.add_argument('reference'); p.add_argument('--executable'); p.add_argument('--compiled',action='store_true'); a=p.parse_args()
r=np.load(a.reference); obs=r['obs']; masks=r['mask']; arrays,prior,digest=load_model(a.model)
root=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='stage3_parity_') as tmp:
    query=Path(tmp)/'queries'; output=Path(tmp)/'outputs'
    with query.open('wb') as f:
        for o,m in zip(obs,masks): f.write(b'MGO1'+struct.pack('<I',256)+o.astype('<i4').tobytes()+m.astype('u1').tobytes())
    subprocess.run([a.executable or str(root/'parity'),'@compiled' if a.compiled else a.model,str(query),str(output)],check=True)
    got=np.frombuffer(output.read_bytes(),dtype='<f4').reshape(len(obs),421)
    assert np.array_equal(got[:,:415],encode(obs)),'encoder mismatch'
    logerr=float(np.abs(got[:,415:420]-r['logits']).max()); valerr=float(np.abs(got[:,420]-r['value']).max())
    assert logerr<2e-4 and valerr<2e-4,(logerr,valerr)
    assert np.array_equal(got[:,415:420].argmax(1),r['logits'].argmax(1)),'argmax drift'
    report=dict(queries=len(obs),weights_sha256=digest,encoder_exact=True,max_logit_error=logerr,max_value_error=valerr,argmax_exact=True)
    Path(a.model+('.header_parity.json' if a.compiled else '.c_parity.json')).write_text(json.dumps(report,indent=2)+'\n'); print(json.dumps(report))
