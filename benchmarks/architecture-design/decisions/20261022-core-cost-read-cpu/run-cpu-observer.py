from pathlib import Path
import subprocess,os,json
repo=Path('/workspace/zyx');dest=repo/'benchmarks/architecture-design/results/20261022-core-cost/cpu-attribution';source=repo/'benchmarks/architecture-design/results/20261020-work-resume/r7-inputs/medium-r43-identity.zyx'
for case in ['label_scan_filter','aggregation_count_by_group']:
 for sample in [True,False]:
  label=case+('-final-sampled' if sample else '-final-control')
  env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu',ZYX_CPU_SAMPLE_OUTPUT=str(dest/(label+'.pcs')))
  if sample:env['ZYX_CPU_SAMPLE_ENABLE']='1'
  cmd=[str(dest/'cpu-probe'),'check-read',str(source),str(dest/(label+'.db')),case,'0','12']
  with (dest/(label+'.jsonl')).open('w') as out,(dest/(label+'.stderr')).open('w') as err:
   p=subprocess.run(cmd,stdout=out,stderr=err,env=env)
  assert p.returncode==0,(label,p.returncode)
  records=[json.loads(s) for s in (dest/(label+'.jsonl')).read_text().splitlines()]
  assert len(records)==12 and all('rows' in r and 'error' not in r for r in records)
  print(label,(dest/(label+'.pcs')).read_text().splitlines()[0],flush=True)
