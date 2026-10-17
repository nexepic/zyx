from pathlib import Path
import argparse,os,sys,json,csv,shutil,importlib.util
spec=importlib.util.spec_from_file_location('window','/tmp/zyx-final-repaired-window.py');w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
ROOT=w.ROOT;CONTROL=w.CONTROL;CURRENT=w.CURRENT;OUT=w.OUT;PREPARED=w.PREPARED;digest=w.digest
def reads(engine,round_number,profile,scale='medium',iterations=10):
 from runner.run import run_benchmark
 from runner.adapters.base import PROFILE_WORKLOADS
 from runner.baseline import BASELINE_WORKLOADS,verify_counts
 from runner.semantic_oracle import verify_observations
 workloads=BASELINE_WORKLOADS[profile]
 if scale=='holdout':workloads={'scan':['label_scan_filter','aggregation_count_by_group','topk_property_sort','two_hop_expand'],'retrieval':['point_node_fetch_by_id']}[profile]
 selected=json.loads(os.environ['ZYX_FINAL_EXTENDED_WORKLOADS'])
 if not selected or not set(selected)<=set(workloads):raise ValueError('Unregistered extended workload')
 workloads=selected
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
p=argparse.ArgumentParser();p.add_argument('--engine',required=True);p.add_argument('--round',type=int,required=True);p.add_argument('--profile',required=True);p.add_argument('--scale',required=True);a=p.parse_args();w.restrict();reads(a.engine,a.round,a.profile,a.scale,100)
