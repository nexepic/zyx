from pathlib import Path
import hashlib,json,subprocess,os,sys
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/current-cpu';decision=repo/'benchmarks/architecture-design/decisions/20261022-core-cost-final';freeze=json.loads((decision/'freeze.json').read_text());sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
assert sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a'))==freeze['release_library_sha256']
sys.path.insert(0,str(decision));from oracle import check_event
for case,kind,iterations in [('label_scan_filter','read',32),('edge_batch_1000','write',12),('property_set','write',50)]:
 source=repo/('benchmarks/architecture-design/results/20261020-work-resume/r7-inputs/medium-r43-identity.zyx' if kind=='read' else 'benchmarks/architecture-design/results/20261022-core-cost/final/inputs/medium-summary.zyx')
 for label in ['control','sampled']:
  stem=dest/(case+'-final-'+label);private=Path('/tmp/zyx-p7-cpu-'+case+'-'+label+'.zyx');assert not private.exists()
  env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu',ZYX_CPU_SAMPLE_OUTPUT=str(stem.with_suffix('.pcs')))
  if label=='sampled':env['ZYX_CPU_SAMPLE_ENABLE']='1'
  cmd=[str(dest/'cpu-probe'),kind,str(source),str(private),case,'0',str(iterations)]
  print('START',case,label,flush=True)
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
   p=subprocess.run(cmd,env=env,stdout=out,stderr=err,timeout=240)
  events=[json.loads(line) for line in stem.with_suffix('.jsonl').read_text().splitlines() if line.startswith('{')]
  assert p.returncode==0 and len(events)==iterations,(case,label,p.returncode,events)
  for e in events:check_event(case,e,'medium',kind=='write')
  stem.with_suffix('.command.json').write_text(json.dumps({'command':cmd,'sampled':label=='sampled','formal_benchmark':False,'purpose':'attribute rejected frozen candidate CPU, not latency acceptance','library_sha256':freeze['release_library_sha256'],'probe_sha256':sha(dest/'cpu-probe')},indent=2)+'\n')
  print('FINISH',case,label,stem.with_suffix('.pcs').read_text().splitlines()[0],flush=True)
