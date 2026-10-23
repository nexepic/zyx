"""Registered baseline: sequential independent workers, preflushed clones, full oracles."""
from pathlib import Path
import json, os, resource, signal, subprocess, sys, time, statistics, csv, math, hashlib
from oracle import check_event, data
HERE=Path(__file__).resolve().parent;REPO=HERE.parents[3]
OLD=REPO/'benchmarks/architecture-design/results/20261020-work-resume'
INPUT_ROOT=REPO/'benchmarks/architecture-design/results/20261022-core-cost'
ROOT=INPUT_ROOT/'acceptance'
OUT=ROOT/'workers';OUT.mkdir(exist_ok=True)
REG=json.loads((HERE/'registration.json').read_text());PYTHON='/workspace/zyx-tools/bin/python'
ENV=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu')
results=[]
def limits():
 resource.setrlimit(resource.RLIMIT_AS,(REG['limits']['address_space_bytes'],)*2)
 resource.setrlimit(resource.RLIMIT_FSIZE,(REG['limits']['file_bytes'],)*2)
def worker(engine,scale,family,cases,round_number,kind='read',input_kind='r7-final-indexed'):
 stem=OUT/f'{round_number}-{scale}-{family}-{engine}';receipt=stem.with_suffix('.json');target=stem.with_suffix('.db')
 if receipt.exists():
  old=json.loads(receipt.read_text());results.append(old)
  if old['status']!='passed':print('RETAINED FAILURE',receipt,flush=True)
  return
 assert not target.exists(),target
 if kind=='import':
  if engine=='zyx':cmd=[str(ROOT/'import-baseline'),str(OLD/f'r7-inputs/{scale}'),str(target),str(sum(data(scale)['counts'].values()))]
  elif family=='import-equal-reopened':cmd=[PYTHON,str(HERE/'kuzu-reopened-import.py'),str(OLD/f'r7-inputs/{scale}'),str(target),scale,str(INPUT_ROOT/(scale+'-chunks'))]
  else:cmd=[PYTHON,str(HERE/'kuzu-import.py'),str(OLD/f'r7-inputs/{scale}'),str(target),scale]
 else:
  source=OLD/f'r7-inputs/{scale}-{input_kind}.zyx' if engine=='zyx' else OLD/f'r7-inputs/{scale}-r7-final.kuzu'
  if engine=='zyx' and kind=='write' and scale in REG['scales']:source=ROOT/'inputs'/f'{scale}-summary.zyx'
  if scale in ['large','skew']:source=OLD/f'r7-inputs/r7-fixtures-{scale}.zyx'
  cmd=([str(ROOT/'public-baseline')] if engine=='zyx' else [PYTHON,str(HERE/'kuzu-worker.py')])+[kind,str(source),str(target),','.join(cases),str(REG['warmup']),str(REG['iterations'])]
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
 record={'engine':engine,'scale':scale,'family':family,'round':round_number,'kind':kind,'status':'failed' if failures else 'passed','failures':failures,'command':cmd,'wall_seconds':time.monotonic()-start,'resources':{'user_seconds':usage.ru_utime,'system_seconds':usage.ru_stime,'peak_rss_bytes':usage.ru_maxrss*1024,'os_read_bytes':usage.ru_inblock*512,'os_write_bytes':usage.ru_oublock*512},'samples':samples}
 record['observed_private_file_peak_bytes']=observed_peak
 record['file_size_observation']='parent polling; may miss short-lived write clones; fresh import emits exact file_bytes'
 record['private_file_bytes'] = sum(path.stat().st_size for path in target.parent.glob(target.name+'*') if path.is_file())
 receipt.write_text(json.dumps(record,indent=2)+'\n');results.append(record)
 (HERE/'final-progress.json').write_text(json.dumps({'completed_processes':len(results),'results':[{'scale':r['scale'],'family':r['family'],'engine':r['engine'],'round':r['round'],'status':r['status']} for r in results]},indent=2)+'\n')
 print('FINISH',round_number,scale,family,engine,record['status'],round(record['wall_seconds'],2),flush=True)
 if failures:print('WORKER FAILURE RETAINED',receipt,failures,flush=True)
 if True:
  # Only this successful worker's private generated artifacts are removed.
  for path in target.parent.glob(target.name+'*'):
   if path.is_file():path.unlink()

def main():
 assert (HERE/'preflight.json').exists()
 freeze=json.loads((HERE/'freeze.json').read_text())
 sha=lambda path: hashlib.file_digest(path.open('rb'),'sha256').hexdigest()
 assert sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a'))==freeze['release_library_sha256']
 for path,expected in freeze['source_files'].items():assert sha(REPO/path)==expected,path
 for name,build in freeze['formal_build'].items():
  binary=Path(build['link'][build['link'].index('-o')+1]);assert sha(binary)==build['binary_sha256']
 for scale in REG['scales']:
  receipt=json.loads((HERE/('final-'+scale+'-summary-init.json')).read_text())
  assert sha(ROOT/'inputs'/f'{scale}-summary.zyx')==receipt['target_sha256']
 for n,order in enumerate(REG['ordering'],1):
  for scale in REG['scales']:
   for family,cases,mode,inp in [('scan',['label_scan_filter','aggregation_count_by_group','multi_key_group','all_nodes_property_filter','one_hop_expand','two_hop_expand'],'read','r43-identity'),('retrieval',['point_lookup_indexed','graph_full_point','graph_full_neighbors'],'read','r7-final-indexed')]+[(c,[c],'write','r7-final-indexed') for c in ['property_set','edge_batch_1000','property_noop','edge_create_delete']]:
    for engine in order:worker(engine,scale,family,cases,n,mode,inp)
   for engine in order:
    worker(engine,scale,'import-production',[],n,'import')
    if engine=='kuzu':worker(engine,scale,'import-equal-reopened',[],n,'import')
   # The ZYX 1000-row import observation is the shared control for both import views.
  for scale in ['large','skew']:
   worker('zyx',scale,'special-read',['large_selected','large_full'] if scale=='large' else ['skew_out','skew_both','skew_twohop'],n)
   if scale=='large':worker('zyx',scale,'large_patch',['large_patch'],n,'write')
 rows={}
 for r in results:
  for e in r['samples']:rows.setdefault((r['scale'],r['family'],e['case'],r['engine']),[]).append(e['latency_ms'])
 with (HERE/'final.csv').open('w',newline='') as f:
  w=csv.writer(f);w.writerow(['scale','family','case','engine','samples','pooled_p50_ms','pooled_p95_ms'])
  for key,values in sorted(rows.items()):w.writerow([*key,len(values),statistics.median(values),sorted(values)[math.ceil(.95*len(values))-1]])
 (HERE/'final-completion.json').write_text(json.dumps({'status':'passed' if all(r['status']=='passed' for r in results) else 'failed','processes':len(results),'implementation_progress_percent':90,'note':'measurement completion is not acceptance; evaluate unchanged P7 gates'},indent=2)+'\n')
if __name__=='__main__':main()
