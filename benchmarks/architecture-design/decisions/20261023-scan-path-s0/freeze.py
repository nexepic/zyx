"""Freeze and verify the accepted production baseline without rerunning timings."""
from pathlib import Path
import csv,hashlib,json,subprocess,sys,os
R=Path(__file__).resolve().parents[4];D=Path(__file__).resolve().parent
OLD=R/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance'
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
def save(n,v):(D/n).write_text(json.dumps(v,ensure_ascii=False,indent=2)+'\n')
f=json.loads((OLD/'freeze.json').read_text());inputs=json.loads((OLD/'input-identities.json').read_text())
checks=[]
for name,ident in inputs.items():
 p=R/name;assert p.stat().st_size==ident['bytes'] and sha(p)==ident['sha256'],name
 checks.append({'path':name,**ident})
for name,h in f['source_files'].items():
 if name.startswith(('src/','include/','cmake/','tests/')):assert sha(R/name)==h,name
lib=Path('/workspace/zyx-build-core-candidate/libzyx_core.a');assert sha(lib)==f['release_library_sha256']
head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip();remote=subprocess.check_output(['git','rev-parse','FETCH_HEAD'],cwd=R,text=True).strip();assert head==remote=='369039dafda8ba6e6764fc7c569ab9b5be7de8b5'
primary=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
rows=list(csv.DictReader((OLD/'comparison.csv').open()))
base=[{'scale':v['scale'],'case':v['case'],'zyx_p50_ms':float(v['final_zyx_p50_ms']),'zyx_p95_ms':float(v['final_zyx_p95_ms']),'kuzu_p50_ms':float(v['current_kuzu_equivalent_p50_ms']),'kuzu_p95_ms':float(v['current_kuzu_p95_ms']),'samples':int(v['samples']),'adoption_maximum_p50_ms':float(v['final_zyx_p50_ms'])/5} for v in rows if v['case'] in primary];assert len(base)==8
registration={'plan':'scan-path-20261023','base_head':head,'release_library_sha256':sha(lib),'primary':primary,'scales':['medium','holdout'],'baseline':base,'final_protocol':{'processes_per_case_engine':3,'warmup':3,'measured':10,'threads':1,'ordering':[['zyx','kuzu'],['kuzu','zyx'],['zyx','kuzu']],'guard_threshold_fraction':0.05,'minimum_speedup_each_primary':5,'no_full_unit_suite':True,'full_matrix':'only once after S4; reuse verified baseline now'},'resources':{'address_space_bytes':6442450944,'registered_file_limit_bytes':2147483648,'worker_timeout_seconds':1200,'raw_buffer_pool_bytes':67108864,'metadata_cache_entries':128,'metadata_cache_charged_bytes':1048576,'conversion_bytes':1048576,'maintenance_workspace_bytes':8388608,'query_budget':'unchanged production defaults; source hashes frozen; no added standalone cache allowance'},'candidate_limits':{'count':2,'correction_rechecks':1,'holdout_policy':'freeze rules before measurement; no holdout tuning','construction_cost':'reported separately, plus first-use total; never hidden from total-cost assessment'},'generality_registration':{'seed':20261023,'owners':16384,'labels':['alpha','beta'],'overlap_every':3,'fields':{'dense_integer':'all owners; deterministic ordinal','sparse_number':'every 16th owner, alternating integer/double','mixed':'integer,text,NULL,missing alternating by ordinal mod4','text':'lengths 0,2,63,1024 by ordinal mod4; include embedded NUL'},'mutations':'update every 8th; remove every 16th; delete every 32nd; label changes every 7th; old pin held and then released','cache_pressure':'same generated input with 4096-byte configured raw cache; no increased budget; not physical cold-disk claim'},'guards':['point_lookup_indexed','graph_full_point','graph_full_neighbors','one_hop_expand','two_hop_expand','property_set','edge_batch_1000','property_noop','edge_create_delete','large_selected','large_full','large_patch','skew_out','skew_both','skew_twohop'],'baseline_reused':True,'baseline_reason':'exact accepted library/source/input identity matched; no environment/input/build evidence requires a new full matrix'}
save('registration.json',registration);save('verified-inputs.json',checks)
save('freeze.json',{'head':head,'fetched_work':remote,'library_sha256':sha(lib),'production_sources':{k:v for k,v in f['source_files'].items() if k.startswith(('src/','include/','cmake/'))},'reference_hashes':{str(p.relative_to(R)):sha(p) for p in [OLD/'comparison.csv',OLD/'registration.json',OLD/'environment.json',OLD/'worker-evidence.tar.gz']},'environment':json.loads((OLD/'environment.json').read_text())})
# Untimed semantic preflight; both engines and both datasets, same production APIs.
sys.path.insert(0,str(OLD));from oracle import check_event
RUN=R/'benchmarks/architecture-design/results/20261023-scan-path';RUN.mkdir(exist_ok=True)
results=[]
for scale in ['medium','holdout']:
 for engine in ['zyx','kuzu']:
  source=R/f'benchmarks/architecture-design/results/20261020-work-resume/r7-inputs/{scale}-r43-identity.zyx'
  if engine=='kuzu':source=source.with_name(f'{scale}-r7-final.kuzu')
  target=RUN/f's0-{scale}-{engine}.db';assert not target.exists()
  exe=[str(R/'benchmarks/architecture-design/results/20261022-core-cost/acceptance/public-baseline')] if engine=='zyx' else ['/workspace/zyx-tools/bin/python',str(OLD/'kuzu-worker.py')]
  cmd=exe+['check-read',str(source),str(target),','.join(primary),'0','1']
  proc=subprocess.run(cmd,env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'),text=True,capture_output=True,timeout=90)
  (D/f'{scale}-{engine}-preflight.jsonl').write_text(proc.stdout);(D/f'{scale}-{engine}-preflight.stderr').write_text(proc.stderr)
  assert proc.returncode==0,proc.stderr
  events=[json.loads(l) for l in proc.stdout.splitlines() if l.startswith('{')];assert len(events)==4
  for e in events:check_event(e['case'],e,scale,False)
  results.append({'scale':scale,'engine':engine,'cases':primary,'oracle':'passed','timed':False,'command':cmd})
  for p in target.parent.glob(target.name+'*'):
   if p.is_file():p.unlink()
  print(scale,engine,'PASS',flush=True)
save('acceptance.json',{'S0':'accepted','progress_percent':5,'input_identities_checked':len(checks),'source_and_library_matched':True,'preflight':results,'new_performance_matrix':False,'unit_suite_run':False})
