from pathlib import Path
import argparse,os,sys,json,hashlib,resource,time,shutil,csv
ROOT=Path('/workspace/zyx');BENCH=ROOT/'benchmarks/graphdb-comparison';sys.path.insert(0,str(BENCH))
CONTROL=Path('/workspace/zyx-optimization-control-20261015/binaries');CURRENT=ROOT/'buildDir_perf_control'
OUT=Path('/workspace/zyx-optimization-final-window-20261016')

def digest(p):
 h=hashlib.sha256()
 with Path(p).open('rb') as f:
  for b in iter(lambda:f.read(1024*1024),b''):h.update(b)
 return h.hexdigest()

def restrict():
 cpu=min(os.sched_getaffinity(0));os.sched_setaffinity(0,{cpu})
 resource.setrlimit(resource.RLIMIT_AS,(6*1024**3,6*1024**3))
 resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024**3,2*1024**3))
 os.environ.update(KUZU_BENCH_BUFFER_POOL_BYTES=str(64*1024**2),KUZU_BENCH_MAX_DB_BYTES=str(2*1024**3),ZYX_COMPARE_EMIT_PROFILE='0',ZYX_CONTRACT_OPERATION_TIMEOUT_SECONDS='1800')

def kuzu_limits():
 import kuzu
 original=kuzu.Database
 class RestrictedDatabase(original):
  def __init__(self,path,**kw):
   kw['max_db_size']=2*1024**3
   super().__init__(path,**kw)
 kuzu.Database=RestrictedDatabase

def preflight(engine,profile,destination,timed=False,seed=42):
 import runner.workload_preflight as f
 if engine=='kuzu':kuzu_limits()
 records=[]
 if engine!='kuzu':
  original=f.NativeWorker.call
  def call(self,*args,**kw):
   begin=time.perf_counter_ns()
   try:return original(self,*args,**kw)
   finally:
    if timed:records.append({'command':args[0],'query':str(args[1])[:240] if len(args)>1 else '', 'ns':time.perf_counter_ns()-begin})
  f.NativeWorker.call=call
 try:
  if engine=='kuzu':report=f.run_kuzu(destination,profile=profile,seed=seed)
  else:report=f.run_native((CONTROL if engine=='control' else CURRENT)/'zyx-workload-contract-worker',destination,profile=profile,seed=seed)
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
 os.environ['ZYX_COMPARE_BENCH']=str((CONTROL if engine=='control' else CURRENT)/'zyx-compare-bench')
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
  OUT.mkdir(exist_ok=False)
  binaries=[CONTROL/'zyx-compare-bench',CONTROL/'zyx-workload-contract-worker',CURRENT/'zyx-compare-bench',CURRENT/'zyx-workload-contract-worker']
  payload={'window':'20261016-final','status':'registered_before_measurement','orders':[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']],'rounds':3,'warmups':3,'samples':10,'tail_sensitive_samples_per_process':1000,'primary_gate_p50_reduction':0.10,'protection_regression_limit':0.05,'stop_minutes_per_case':30,'extended_sampling':'at most two independent 100-sample processes when preregistered uncertainty rule applies','resources':{'affinity':'one allowed CPU','address_space_bytes':6*1024**3,'per_file_bytes':2*1024**3,'kuzu_pool_bytes':64*1024**2,'kuzu_mapping_bytes':2*1024**3,'threads_per_query':1,'native_query_materialization_bytes':64*1024**2,'durability':'default sync and acknowledgements; never disabled','os_cache':'warm; no cold-cache claim'},'resource_notes':['Per-file and Kuzu virtual database mapping caps are stricter than the production 1TiB maximum; the identical 2GiB OS per-file cap applies to every engine.','6GiB address-space cap implies a physical resident cap but is not a container/RSS accounting equivalence. Native charged class budgets remain separately reported.'],'binaries':{str(v):digest(v) for v in binaries},'contract_sha256':digest(ROOT/'benchmarks/architecture-design/decisions/20261014-s0-workload-contract/contract.json'),'source_proof':'benchmarks/architecture-design/decisions/20261016-s61-final-source-acceptance/proof.json','script_sha256':digest(__file__)}
  (OUT/'registration.json').write_text(json.dumps(payload,indent=2)+'\n');print('Registered final window',flush=True)
 else:
  restrict()
  if a.mode in ['smoke','contract']:preflight(a.engine,a.mode,OUT/a.mode/f'round-{a.round}'/a.engine,timed=a.mode=='contract')
  else:reads(a.engine,a.round,a.profile,a.scale,a.iterations)
