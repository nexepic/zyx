from pathlib import Path
import sys,json,subprocess,hashlib
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design';sys.path.insert(0,str(root/'decisions/20261025-final-scan-comparison'));import campaign
out=campaign.INPUT_ROOT/'s4-native-write-cost'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
for scale in ['medium','holdout']:
 source=campaign.INPUT_ROOT/f'{scale}.scan-corrected-v1.zyx';before=sha(source)
 for case in ['edge_batch_1000','edge_create_delete']:
  stem=out/f'public-{scale}-{case}';target=stem.with_suffix('.db');assert not target.exists();cmd=[str(out/'values-b'),'check-write',str(source),str(target),case,'0','1']
  print('START untimed',scale,case,flush=True)
  with stem.with_suffix('.jsonl').open('w') as stdout,stem.with_suffix('.stderr').open('w') as stderr:r=subprocess.run(cmd,env=campaign.ENV,stdout=stdout,stderr=stderr,preexec_fn=campaign.limits,timeout=1200)
  assert r.returncode==0,(stem,r.returncode)
  events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(events)==1
  assert 'latency_ms' not in events[0];campaign.check_event(case,events[0],scale,True);assert sha(source)==before
  stem.with_suffix('.json').write_text(json.dumps({'status':'passed','query_benchmark':False,'full_oracle':True,'source_sha256':before,'command':cmd},indent=2)+'\n')
  for path in target.parent.glob(target.name+'*'):
   if path.is_file():path.unlink()
  print('PASS untimed',scale,case,flush=True)
