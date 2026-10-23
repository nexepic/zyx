from pathlib import Path
import subprocess,json,hashlib,os,math,statistics,sys,tarfile,resource
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance';o=r/'benchmarks/architecture-design/results/20261022-core-cost/acceptance/guard-confirmation';o.mkdir(exist_ok=True)
sys.path.insert(0,str(d));from oracle import check_event
base=r/'benchmarks/architecture-design/results/20261022-core-cost/public-baseline';current=r/'benchmarks/architecture-design/results/20261022-core-cost/acceptance/public-baseline';sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
assert sha(base)==json.loads((r/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/build.json').read_text())['public']['sha256']
protocol={'reason':'Final original matrix has3 guard P95 exceedances with no P50 regression; verify stability with contemporaneous frozen-baseline controls. Do not replace original samples.','cases':['large_selected','large_patch','skew_twohop'],'processes_per_engine_case':3,'warmups':3,'measurements':30,'order':[['baseline','current'],['current','baseline'],['baseline','current']],'threshold':1.05,'baseline_binary_sha256':sha(base),'current_binary_sha256':sha(current),'full_matrix_rerun':False}
(d/'guard-confirmation-protocol.json').write_text(json.dumps(protocol,indent=2)+'\n')
results=[]
def limits():
 resource.setrlimit(resource.RLIMIT_AS,(6442450944,)*2);resource.setrlimit(resource.RLIMIT_FSIZE,(2147483648,)*2)
for n,order in enumerate(protocol['order'],1):
 for case in protocol['cases']:
  scale='skew' if case=='skew_twohop' else 'large';mode='write' if case=='large_patch' else 'read';source=r/f'benchmarks/architecture-design/results/20261020-work-resume/r7-inputs/r7-fixtures-{scale}.zyx'
  for engine in order:
   stem=o/f'{n}-{case}-{engine}';target=stem.with_suffix('.db');assert not target.exists()
   cmd=[str(base if engine=='baseline' else current),mode,str(source),str(target),case,'3','30']
   with stem.with_suffix('.jsonl').open('w') as stdout,stem.with_suffix('.stderr').open('w') as stderr:p=subprocess.run(cmd,env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'),stdout=stdout,stderr=stderr,timeout=120,preexec_fn=limits)
   assert p.returncode==0,(cmd,p.returncode)
   samples=[json.loads(x) for x in stem.with_suffix('.jsonl').read_text().splitlines() if x.startswith('{')];samples=[v for v in samples if 'latency_ms' in v];assert len(samples)==30
   for value in samples:check_event(case,value,scale,mode=='write')
   record={'round':n,'case':case,'engine':engine,'command':cmd,'samples':samples};results.append(record);stem.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n')
   for path in target.parent.glob(target.name+'*'):
    if path.is_file():path.unlink()
summary=[]
for case in protocol['cases']:
 item={'case':case};values={}
 for engine in ['baseline','current']:
  v=[s['latency_ms'] for q in results if q['case']==case and q['engine']==engine for s in q['samples']];values[engine]={'samples':len(v),'p50_ms':statistics.median(v),'p95_ms':sorted(v)[math.ceil(.95*len(v))-1]}
 item.update(values);item['p50_ratio']=values['current']['p50_ms']/values['baseline']['p50_ms'];item['p95_ratio']=values['current']['p95_ms']/values['baseline']['p95_ms'];item['within_5_percent']=item['p50_ratio']<=1.05 and item['p95_ratio']<=1.05;summary.append(item)
(d/'guard-confirmation.json').write_text(json.dumps({'protocol':protocol,'all_oracles_passed':True,'processes':len(results),'summary':summary,'original_matrix_unchanged':True},indent=2)+'\n')
with tarfile.open(d/'guard-confirmation-evidence.tar.gz','w:gz') as t:
 for path in sorted(o.iterdir()):
  if path.is_file():t.add(path,arcname=path.name)
print(json.dumps(summary,indent=2))
