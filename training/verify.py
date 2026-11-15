"""Compile, check frozen legacy code/run1, and run all acceptance fixtures."""
from pathlib import Path
import hashlib,subprocess,difflib,json,os
ROOT=Path(__file__).resolve().parent.parent
S=ROOT/'stage3'
def run(cmd,**kw): return subprocess.run(cmd,cwd=ROOT,check=True,**kw)
for name,src,flags in [('engine','engine.c',['-O3']),('stage3/loop','stage3/loop.c',['-O3']),('stage3/parity','stage3/parity.c',['-O3']),('stage3/sampling_test','stage3/sampling_test.c',['-O2']),('stage3/observation_test','observation_test.c',['-O2'])]:
    run(['gcc',*flags,'-Wall','-Wextra','-Werror','-o',name,src,'-lm'])
for label,env in [('disabled',{}),('compat',dict(ENGINE_OBSERVER='1',ENGINE_OBS_COMPAT='1'))]:
    clean={k:v for k,v in os.environ.items() if not k.startswith('ENGINE_')}; clean.update(env)
    with (S/f'matrix_{label}.txt').open('wb') as f: run(['./engine','matrix','3000','12345'],stdout=f,env=clean)
    assert hashlib.md5((S/f'matrix_{label}.txt').read_bytes()).hexdigest()=='21f0b28488d25682bb05ceba5ef5d8b6'
with (S/'sampling_test.txt').open('wb') as out: run(['stage3/sampling_test'],stdout=out)
with (S/'observation_test.txt').open('wb') as out,(S/'observation_test.stderr').open('wb') as err: run(['stage3/observation_test'],stdout=out,stderr=err)
run(['gcc','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-fno-omit-frame-pointer','-o','stage3/observation_test_sanitized','observation_test.c','-lm'])
with (S/'observation_sanitized.txt').open('wb') as out,(S/'observation_sanitized.stderr').open('wb') as err:
    run(['stage3/observation_test_sanitized'],stdout=out,stderr=err,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
with (S/'run1_integrity.txt').open('wb') as out: run(['sha256sum','-c','stage3/run1_before.sha256'],stdout=out)
before=(S/'engine_before.c').read_text(); after=(ROOT/'engine.c').read_text()
def function(src,name):
    # Full function including balanced braces; comments here have no brace literals.
    import re
    match=re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\) \{',src,re.M)
    if not match: raise ValueError(name)
    start=match.start(); i=match.end(); depth=1
    while depth:
        if src[i]=='{': depth+=1
        if src[i]=='}': depth-=1
        i+=1
    return src[start:i]
frozen=['choose_move','calc_damage','est_damage','execute_move','end_of_turn','rng_next','obs_build','obs_mask','obs_replay','obs_emit','obs_hp','obs_finish','obsh2','obsdemo']
checks={name:function(before,name)==function(after,name) for name in frozen}; assert all(checks.values())
start=before.index('static const MoveDef MOVES'); end=before.index('static int move_from_code',start)
checks['MOVES']=before[start:end]==after[after.index('static const MoveDef MOVES'):after.index('static int move_from_code')]; assert checks['MOVES']
(S/'frozen_code_audit.json').write_text(json.dumps(checks,indent=2)+'\n')
(S/'engine.diff').write_text(''.join(difflib.unified_diff(before.splitlines(True),after.splitlines(True),fromfile='stage2/engine.c',tofile='engine.c')))
print('both required matrix hashes, full harness, ASan/UBSan, frozen code and run1 integrity PASS')
