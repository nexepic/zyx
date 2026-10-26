"""Single final window; run only after --freeze manifests and completed images exist."""
from pathlib import Path
import json,os,resource,signal,subprocess,sys,time,statistics,csv,math,hashlib
REPO=Path('/workspace/zyx')
HERE=REPO/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance'
sys.path.insert(0,str(HERE))
import oracle
INPUT_ROOT=REPO/'benchmarks/architecture-design/results/20261024-work-scan'
ROOT=REPO/'benchmarks/architecture-design/results/20261024-work-scan/final-comparison'
OUT=ROOT/'workers'; OUT.mkdir(parents=True,exist_ok=True)
links=ROOT/'r7-inputs';links.mkdir(exist_ok=True)
for scale in ['medium','holdout']:
 p=links/scale
 if not p.exists():p.symlink_to(INPUT_ROOT/scale,target_is_directory=True)
oracle.ROOT=ROOT
check_event=oracle.check_event;data=oracle.data
PYTHON='/workspace/zyx-tools/bin/python'
REG={'warmup':3,'iterations':10,'limits':{'address_space_bytes':6*1024**3,'file_bytes':2*1024**3,'process_timeout_seconds':1200}}
ENV=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu')
BINARIES={'initial':str(INPUT_ROOT/'public-baseline'),'control':'/tmp/zyx-frozen-control-public','A':'/tmp/zyx-final-a-public','B':'/tmp/zyx-final-b-public'}
results=[]
B_IMAGES={}
FREEZE_ID=None
def limits():
 resource.setrlimit(resource.RLIMIT_AS,(REG['limits']['address_space_bytes'],)*2)
 resource.setrlimit(resource.RLIMIT_FSIZE,(REG['limits']['file_bytes'],)*2)
def worker(engine,scale,family,cases,round_number,kind='read',input_kind='r7-final-indexed'):
 stem=OUT/f'{round_number}-{scale}-{family}-{engine}';receipt=stem.with_suffix('.json');target=stem.with_suffix('.db')
 if receipt.exists():
  old=json.loads(receipt.read_text())
  assert old.get('freeze_sha256')==FREEZE_ID,('receipt belongs to another frozen campaign',receipt)
  results.append(old)
  if old['status']!='passed':print('RETAINED FAILURE',receipt,flush=True)
  return
 assert not target.exists(),target
 if kind=='import':
  if engine=='zyx':cmd=[str(ROOT/'import-baseline'),str(OLD/f'r7-inputs/{scale}'),str(target),str(sum(data(scale)['counts'].values()))]
  elif family=='import-equal-reopened':cmd=[PYTHON,str(HERE/'kuzu-reopened-import.py'),str(OLD/f'r7-inputs/{scale}'),str(target),scale,str(INPUT_ROOT/(scale+'-chunks'))]
  else:cmd=[PYTHON,str(HERE/'kuzu-import.py'),str(OLD/f'r7-inputs/{scale}'),str(target),scale]
 else:
  if scale in ['large','skew']:
   source=INPUT_ROOT/('guard-frozen-large.zyx' if scale=='large' else 'guard-skew-indexed.zyx')
  else:
   source=B_IMAGES[scale] if engine=='B' else INPUT_ROOT/(scale+('.kuzu' if engine=='kuzu' else '.zyx'))
  cmd=([PYTHON,str(HERE/'kuzu-worker.py')] if engine=='kuzu' else [BINARIES[engine]])+[kind,str(source),str(target),','.join(cases),str(REG['warmup']),str(REG['iterations'])]
 print('START',round_number,scale,family,engine,flush=True);start=time.monotonic()
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
  p=subprocess.Popen(cmd,env=ENV,stdout=out,stderr=err,preexec_fn=limits,start_new_session=True)
  timedout=False;observed_peak=0
  while True:
   try:observed_peak=max(observed_peak,target.stat().st_size)
   except FileNotFoundError:pass
   pid,status,usage=os.wait4(p.pid,os.WNOHANG)
   if pid:break
   if time.monotonic()-start>REG['limits']['process_timeout_seconds']:
    timedout=True;os.killpg(p.pid,signal.SIGKILL);pid,status,usage=os.wait4(p.pid,0);break
   time.sleep(.2)
  p.returncode=os.waitstatus_to_exitcode(status)
 events=[];failures=[]
 try:
  assert p.returncode==0 and not timedout,('worker failure',p.returncode,timedout)
  events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')]
  assert not any(e.get('status')=='failed' for e in events),events
  if kind=='import':
   samples=[e for e in events if e.get('case')=='fresh_import'];assert len(samples)==1
   d=data(scale)['counts'];e=samples[0]
   assert e['loaded_rows']==sum(d.values()) and e['nodes']==d['users']+d['posts']+d['tags'] and e['edges']==d['follows']+d['authored']+d['has_tag']
  else:
   samples=[e for e in events if 'latency_ms' in e]
   for case in cases:
    found=[e for e in samples if e['case']==case];assert len(found)==REG['iterations'],(case,len(found))
    for e in found:check_event(case,e,scale,kind=='write')
  assert all(math.isfinite(e['latency_ms']) and e['latency_ms']>=0 for e in samples)
 except Exception as e:failures.append(repr(e));samples=[]
 record={'freeze_sha256':FREEZE_ID,'engine':engine,'scale':scale,'family':family,'round':round_number,'kind':kind,'status':'failed' if failures else 'passed','failures':failures,'command':cmd,'wall_seconds':time.monotonic()-start,'resources':{'user_seconds':usage.ru_utime,'system_seconds':usage.ru_stime,'peak_rss_bytes':usage.ru_maxrss*1024,'os_read_bytes':usage.ru_inblock*512,'os_write_bytes':usage.ru_oublock*512},'samples':samples}
 grouped={}
 for event in samples:grouped.setdefault(event['case'],[]).append(event['latency_ms'])
 record['case_statistics']={case:{'samples':len(values),'p50_ms':statistics.median(values),'p95_ms':sorted(values)[math.ceil(.95*len(values))-1]} for case,values in grouped.items()}
 record['observed_private_file_peak_bytes']=observed_peak
 record['file_size_observation']='parent polling; may miss short-lived write clones; fresh import emits exact file_bytes'
 record['private_file_bytes'] = sum(path.stat().st_size for path in target.parent.glob(target.name+'*') if path.is_file())
 receipt.write_text(json.dumps(record,indent=2)+'\n');results.append(record)
 (ROOT/'progress.json').write_text(json.dumps({'completed_processes':len(results),'results':[{'scale':r['scale'],'family':r['family'],'engine':r['engine'],'round':r['round'],'status':r['status']} for r in results]},indent=2)+'\n')
 print('FINISH',round_number,scale,family,engine,record['status'],round(record['wall_seconds'],2),flush=True)
 if failures:print('WORKER FAILURE RETAINED',receipt,failures,flush=True)
 if not failures:
  # Only this successful worker's private generated artifacts are removed.
  for path in target.parent.glob(target.name+'*'):
   if path.is_file():path.unlink()

def main():
 global FREEZE_ID
 freeze_path=ROOT/'freeze.json'
 FREEZE_ID=hashlib.sha256(freeze_path.read_bytes()).hexdigest()
 freeze=json.loads(freeze_path.read_text())
 B_IMAGES.update({scale:Path(freeze['artifacts'][scale+'_scan']['path']) for scale in ['medium','holdout']})
 for relative,expected in json.loads((ROOT/'source-files.json').read_text()).items():
  assert hashlib.file_digest((REPO/relative).open('rb'),'sha256').hexdigest()==expected,relative
 for name,entry in freeze['artifacts'].items():
  p=Path(entry['path']);assert hashlib.file_digest(p.open('rb'),'sha256').hexdigest()==entry['sha256'],name
 scans=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
 retrieval=['point_lookup_indexed','graph_full_point','graph_full_neighbors','one_hop_expand','two_hop_expand']
 for n in range(3):
  order=['initial','control','A','B','kuzu'] if n%2==0 else ['kuzu','B','A','control','initial']
  for scale in ['medium','holdout']:
   for engine in order:worker(engine,scale,'scan',scans,n)
   for family,cases,mode in [('retrieval',retrieval,'read')]+[(c,[c],'write') for c in ['property_set','edge_batch_1000','property_noop','edge_create_delete']]:
    for engine in [e for e in order if e!='kuzu']:worker(engine,scale,family,cases,n,mode)
  for scale in ['large','skew']:
   for engine in ['initial','control','A'] if n%2==0 else ['A','control','initial']:
    worker(engine,scale,'special-read',['large_selected','large_full'] if scale=='large' else ['skew_out','skew_both','skew_twohop'],n)
    if scale=='large':worker(engine,scale,'large_patch',['large_patch'],n,'write')
 # Work observations are untimed, after every timed worker has finished.
 for scale in ['medium','holdout']:
  for engine,tag,suffix in [('A','a','.zyx'),('B','b',None)]:
   stem=OUT/f'values-{scale}-{engine}';target=stem.with_suffix('.db')
   assert not target.exists()
   command=[f'/tmp/zyx-final-{tag}-values','check-read',str(B_IMAGES[scale] if engine=='B' else INPUT_ROOT/(scale+suffix)),str(target),','.join(scans),'0','1']
   with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
    observation=subprocess.run(command,env=ENV,stdout=out,stderr=err,preexec_fn=limits,timeout=1200)
   assert observation.returncode==0,stem
   events=[json.loads(line) for line in stem.with_suffix('.jsonl').read_text().splitlines() if line.startswith('{')]
   assert len(events)==len(scans) and not any(e.get('status')=='failed' for e in events),stem
   for event in events:check_event(event['case'],event,scale)
   assert not target.exists(),target
 rows={}
 for r in results:
  for e in r['samples']:rows.setdefault((r['scale'],r['family'],e['case'],r['engine']),[]).append(e['latency_ms'])
 with (ROOT/'summary.csv').open('w',newline='') as f:
  w=csv.writer(f);w.writerow(['scale','family','case','engine','samples','p50_ms','p95_ms'])
  for key,v in sorted(rows.items()):w.writerow([*key,len(v),statistics.median(v),sorted(v)[math.ceil(.95*len(v))-1]])
 with (ROOT/'process-summary.csv').open('w',newline='') as f:
  w=csv.writer(f);w.writerow(['scale','family','case','engine','round','samples','p50_ms','p95_ms'])
  for record in results:
   for case,stats in sorted(record['case_statistics'].items()):w.writerow([record['scale'],record['family'],case,record['engine'],record['round'],stats['samples'],stats['p50_ms'],stats['p95_ms']])
 (ROOT/'completion.json').write_text(json.dumps({'all_workers_passed':all(r['status']=='passed' for r in results),'processes':len(results),'expected_processes':177,'measured_samples':sum(len(r['samples']) for r in results),'expected_measured_samples':3900,'acceptance':'evaluate frozen per-case gates separately'},indent=2)+'\n')
if __name__=='__main__':main()
