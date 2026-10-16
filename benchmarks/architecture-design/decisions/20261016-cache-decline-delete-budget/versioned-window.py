from pathlib import Path
import argparse,os,sys,json,hashlib,resource,time,shutil,csv
ROOT=Path('/workspace/zyx');BENCH=ROOT/'benchmarks/graphdb-comparison';sys.path.insert(0,str(BENCH))
CONTROL=Path('/workspace/zyx-optimization-control-20261015/binaries');CURRENT=ROOT/'buildDir_perf_control'
OUT=Path(os.environ.get('ZYX_CONTRACT_OUTPUT_ROOT','/workspace/zyx-optimization-final-window-20261016/bounded-loader-v3'))
PREPARED=Path('/workspace/zyx-optimization-final-window-20261016/prepared-inputs')

def digest(p):
 h=hashlib.sha256()
 with Path(p).open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()

def restrict():
 cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
 resource.setrlimit(resource.RLIMIT_AS,(6*1024**3,6*1024**3))
 resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024**3,2*1024**3))
 os.environ.update(KUZU_BENCH_BUFFER_POOL_BYTES=str(64*1024**2),KUZU_BENCH_MAX_DB_BYTES=str(2*1024**3),ZYX_COMPARE_EMIT_PROFILE='0',ZYX_CONTRACT_OPERATION_TIMEOUT_SECONDS='1800',ZYX_COMPARE_LOAD_BATCH_ROWS='1000',ZYX_COMPARE_PROFILE_PROCESS='1',ZYX_COMPARE_TIMEOUT_SECONDS='1800')

def kuzu_limits():
 import kuzu
 original=kuzu.Database
 class RestrictedDatabase(original):
  def __init__(self,path,**kw):
   kw['max_db_size']=2*1024**3
   super().__init__(path,**kw)
 kuzu.Database=RestrictedDatabase

def preflight(engine,profile,destination,timed=False,seed=42,family=None):
 import runner.workload_preflight as f
 if engine=='kuzu':kuzu_limits()
 records=[]
 if engine!='kuzu':
  original=f.NativeWorker.call
  def call(self,*args,**kw):
   begin=time.perf_counter_ns();failure=None
   try:return original(self,*args,**kw)
   except BaseException as error:
    failure=str(error);raise
   finally:
    if timed:records.append({'command':args[0],'query':str(args[1])[:240] if len(args)>1 else '', 'family':getattr(getattr(self,'family_budget',None),'current_family',None), 'sampling_phase':getattr(getattr(self,'family_budget',None),'sampling_phase','setup'), 'ns':time.perf_counter_ns()-begin,'failed':failure is not None,'error':failure})
  f.NativeWorker.call=call
 try:
  if engine=='kuzu':report=f.run_kuzu(destination,profile=profile,seed=seed,family=family)
  else:
   binary=(CONTROL if engine=='control' else CURRENT)/'zyx-workload-contract-worker'
   frozen=os.environ.get('ZYX_FROZEN_BASELINE_WORKER')
   if frozen and engine=='current':
    registration=json.loads((OUT/'registration.json').read_text())
    assert digest(frozen)==registration['binaries'][str(binary)], 'frozen baseline binary differs from original registration'
    binary=Path(frozen)
   report=f.run_native(binary,destination,profile=profile,seed=seed,family=family)
 finally:
  if timed:
   destination.mkdir(parents=True,exist_ok=True);(destination/'ipc-operation-timings.json').write_text(json.dumps({'scope':'Native IPC including full result serialization, decoding and semantic validation; separate from native benchmark query timings','operations':records},indent=2)+'\n')
 print(json.dumps({'engine':engine,'profile':profile,'report':report}),flush=True)

def reads(engine,round_number,profile,scale='medium',iterations=10):
 from runner.run import run_benchmark
 from runner.adapters.base import PROFILE_WORKLOADS
 from runner.baseline import BASELINE_WORKLOADS,verify_counts
 from runner.semantic_oracle import verify_observations
 workloads=BASELINE_WORKLOADS[profile]
 if scale=='holdout':workloads={'scan':['label_scan_filter','aggregation_count_by_group','topk_property_sort','two_hop_expand'],'retrieval':['point_node_fetch_by_id']}[profile]
 PROFILE_WORKLOADS[profile]=workloads
 actual='kuzu' if engine=='kuzu' else 'zyx'
 os.environ['ZYX_COMPARE_BENCH']=str(CONTROL/'zyx-compare-bench-bounded' if engine=='control' else CURRENT/'zyx-compare-bench')
 if engine!='kuzu':os.environ['ZYX_COMPARE_PREPARED_DB']=str(PREPARED/scale/f'{engine}.zyx')
 else:os.environ.pop('ZYX_COMPARE_PREPARED_DB',None)
 folder=OUT/'reads'/scale/f'round-{round_number}'/engine/profile
 result=run_benchmark([actual],scale,314159 if scale=='holdout' else 42,folder,3,iterations,profile=profile,threads=1,observe_result_rows=True)
 hashes={p.name:digest(p) for p in (result/'dataset').iterdir() if p.is_file()}
 (result/'dataset-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
 checks=verify_counts(result,workloads,iterations,[actual])
 with (result/'dataset/users.csv').open() as stream:users=list(csv.DictReader(stream))
 events=[json.loads(v) for v in (result/'raw.jsonl').read_text().splitlines()]
 full=verify_observations(users,events,[actual],workloads,iterations)
 (result/'correctness.json').write_text(json.dumps({'scalar':checks,'full_results':full},indent=2)+'\n')
 status=json.loads((result/'run_status.json').read_text())
 shutil.rmtree(result/'dataset')
 print(json.dumps({'engine':engine,'round':round_number,'profile':profile,'scale':scale,'status':status,'oracle_failures':checks['failures']+full['failures'],'output':str(result)}),flush=True)
 if status['failure_count'] or status['quality_failure_count'] or checks['failures'] or full['failures']:raise SystemExit(1)

if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('mode',choices=['register','smoke','contract','read']);p.add_argument('--engine',choices=['control','current','kuzu']);p.add_argument('--round',type=int,default=1);p.add_argument('--profile',default='scan');p.add_argument('--scale',default='medium');p.add_argument('--iterations',type=int,default=10);a=p.parse_args()
 if a.mode=='register':
  fixtures={}
  for scale in ['medium','holdout']:
   for engine in ['control','current']:
    path=PREPARED/scale/f'{engine}.zyx'
    binary=CONTROL/'zyx-compare-bench-bounded' if engine=='control' else CURRENT/'zyx-compare-bench'
    failure=PREPARED/scale/f'{engine}-preparation-failure.json'
    if failure.exists():
     assert engine=='control', 'current preparation must succeed before final timing'
     manifest=json.loads(failure.read_text());assert manifest['status']=='preparation_failed' and manifest['formal_timing_samples_accepted']==0
    else:manifest=json.loads(Path(str(path)+'.json').read_text())
    assert manifest['consumer_binary_sha256']==digest(binary) and manifest['database_sha256']==digest(path)
    fixtures[str(path)]=manifest
  OUT.mkdir(exist_ok=False)
  binaries=[CONTROL/'zyx-compare-bench-bounded',CONTROL/'zyx-workload-contract-worker',CURRENT/'zyx-compare-bench',CURRENT/'zyx-workload-contract-worker',Path('/tmp/zyx-mechanism-probe')]
  payload={'window':'20261016-final-bounded-loader-v3','continuation_reason':'v1 stopped at preparation; v2 excluded after reproduced default Top-K memory failure. This window uses the repaired bounded selector and final revalidated source.','preparation_transaction_rows':1000,'native_read_process_scope':'one dataset preparation per independent profile process, exact selected query list; each query retains three warmups and ten owning-output samples','status':'registered_before_measurement','orders':[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']],'rounds':3,'warmups':3,'samples':10,'tail_sensitive_samples_per_process':1000,'primary_gate_p50_reduction':0.10,'protection_regression_limit':0.05,'stop_minutes_per_case':30,'contract_case_unit':'one isolated workload family in a private process, at most 30 minutes including setup; failed/inconclusive families retained independently','extended_sampling':'at most two independent 100-sample processes when preregistered uncertainty rule applies','resources':{'affinity':'one allowed CPU','address_space_bytes':6*1024**3,'per_file_bytes':2*1024**3,'kuzu_pool_bytes':64*1024**2,'kuzu_mapping_bytes':2*1024**3,'threads_per_query':1,'native_query_materialization_bytes':64*1024**2,'durability':'default sync and acknowledgements; never disabled','os_cache':'warm; no cold-cache claim'},'resource_notes':['Per-file and Kuzu virtual database mapping caps are stricter than the production 1TiB maximum; the identical 2GiB OS per-file cap applies to every engine.','6GiB address-space cap implies a physical resident cap but is not a container/RSS accounting equivalence. Native charged class budgets remain separately reported.'],'binaries':{str(v):digest(v) for v in binaries},'contract_sha256':digest(ROOT/'benchmarks/architecture-design/decisions/20261014-s0-workload-contract/contract.json'),'source_proof':'benchmarks/architecture-design/decisions/20261016-s61-final-source-acceptance/proof.json','script_sha256':digest(__file__)}
  payload['isolated_family_policy']={'maintenance':'three warmup windows excluded, then ten measured windows with 100 durable batch inserts each; 1000 measured insert observations per process','other_families':'full-size semantic and capability observations; one-pass operation timings do not satisfy repeated latency gates; after an unchanged family fails or exhausts 30 minutes, later rounds record the same unmeasurable dependency without repeating the failed workload','prior_dynamic_attempt':'30-minute cutoff, inconclusive; no full-size all-family pass assumed','ordering':'maintenance first, then documented family order; three rotating engine orders per family'}
  payload['known_native_budget_limits']={'aggregate_100000_groups_owning_output':'Observed fixed QUERY-budget rejection. Validate every typed group/count via bounded partitions and preserve the full-output denial as unsupported N/A; no successful latency sample or ratio is inferred. Skewed groups and other failures are not exempt.'}
  payload['untimed_fixture_protocol']='Committed-prefix continuation is disclosed in each fixture manifest. Original prefixes use 1000-row transactions; remaining fixture-only suffixes may use bounded 5000-row transactions. No import timing or fresh final-version import claim; final consumers are revalidated before use.'
  payload['prepared_inputs']=fixtures
  payload['attribution_registration']=json.loads((ROOT/'benchmarks/architecture-design/decisions/20261016-final-performance-window/bounded-loader-v3/attribution-collector-policy.json').read_text())
  payload['preflight_runner_sha256']=digest(ROOT/'benchmarks/graphdb-comparison/runner/workload_preflight.py')
  payload['frozen_control_failure_policy']='A reproduced frozen-control import failure is retained with exact error and input/binary hashes, omitted from latency measurements and ratios, and never silently treated as success. Current/Kuzu supported cases continue.'
  payload['prepared_read_protocol']='immutable input from each corresponding engine; exact binary/CSV/database hashes checked before every independent process; private writable clone; missing requested indexes only; no import/write timing from prepared input'
  payload['hardware']={'platform':__import__('platform').platform(),'machine':__import__('platform').machine(),'cpu_count':os.cpu_count(),'allowed_cpus_before_restriction':sorted(os.sched_getaffinity(0)),'cpu_model':next((v.split(':',1)[1].strip() for v in Path('/proc/cpuinfo').read_text().splitlines() if v.startswith('model name')),None),'cgroup_memory_max':Path('/sys/fs/cgroup/memory.max').read_text().strip() if Path('/sys/fs/cgroup/memory.max').exists() else None,'cgroup_cpu_max':Path('/sys/fs/cgroup/cpu.max').read_text().strip() if Path('/sys/fs/cgroup/cpu.max').exists() else None}
  (OUT/'registration.json').write_text(json.dumps(payload,indent=2)+'\n');print('Registered final window',flush=True)
 else:
  restrict()
  if a.mode in ['smoke','contract']:preflight(a.engine,a.mode,OUT/a.mode/f'round-{a.round}'/a.engine,timed=a.mode=='contract')
  else:reads(a.engine,a.round,a.profile,a.scale,a.iterations)
