from pathlib import Path
from collections import defaultdict
import json,statistics,csv,math
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v3')
assert (root/'read-campaign-completed.json').exists()
# A separate, untimed capability witness explains the failed setup without
# relabeling primary-key lookup as unsupported or inventing latency samples.
capability=root/'kuzu-secondary-index-capability.json'
if not capability.exists():
 import importlib.util
 spec=importlib.util.spec_from_file_location('window','/tmp/zyx-final-window.py')
 window=importlib.util.module_from_spec(spec);spec.loader.exec_module(window);window.restrict()
 import kuzu
 from runner.adapters.kuzu import supports_secondary_property_indexes
 capability.write_text(json.dumps({'engine':'kuzu','version':kuzu.__version__,'secondary_property_index_supported':supports_secondary_property_indexes(),'primary_key_lookup':'supported, separate from scalar secondary indexes','limits':{'buffer_pool_bytes':64*1024**2,'max_database_mapping_bytes':2*1024**3,'threads':1},'formal_timing_samples':0},indent=2)+'\n')

groups=defaultdict(dict)
def percentile(values,p):
 values=sorted(values);index=(len(values)-1)*p;lo=math.floor(index);hi=math.ceil(index)
 return values[lo]+(values[hi]-values[lo])*(index-lo)
for path in sorted((root/'reads').glob('*/*/*/*/*/raw.jsonl')):
 scale,round_name,engine,profile=path.relative_to(root/'reads').parts[:4]
 status=json.loads((path.parent/'run_status.json').read_text())
 if status['failure_count'] or status['quality_failure_count']:continue
 oracle=json.loads((path.parent/'correctness.json').read_text());assert not oracle['scalar']['failures'] and not oracle['full_results']['failures']
 samples=defaultdict(list)
 for line in path.read_text().splitlines():
  event=json.loads(line)
  if event.get('event')=='sample':
   assert event['status']=='ok';samples[event['workload']].append(event['latency_ms'])
 for workload,values in samples.items():
  assert len(values)==10,(path,workload,len(values))
  groups[(scale,profile,workload,engine)][round_name]={'p50_ms':statistics.median(values),'p95_ms':percentile(values,.95),'samples':len(values),'source':str(path)}
rows=[]
requested=set(key[:3] for key in groups)
requested.update(('medium','indexed',v) for v in ['point_lookup_indexed','property_equality_indexed','property_range_indexed'])
for scale,profile,workload in sorted(requested):
 engines={}
 for engine in ['control','current','kuzu']:
  processes=groups[(scale,profile,workload,engine)]
  failed=list((root/'reads'/scale).glob(f'round-*/{engine}/{profile}/campaign-case-failed.json'))
  if not processes and failed:
   engines[engine]={'status':'failed_or_inconclusive','cases':[json.loads(v.read_text()) for v in failed]};continue
  if not processes and engine=='control':
   failure=root/'reads'/scale/'round-1'/engine/profile/'preparation-failure.json';assert failure.exists()
   engines[engine]={'status':'preparation_failed','failure':json.loads(failure.read_text())};continue
  assert len(processes)==3
  values=[v['p50_ms'] for v in processes.values()]
  engines[engine]={'median_process_p50_ms':statistics.median(values),'min_process_p50_ms':min(values),'max_process_p50_ms':max(values),'processes':processes}
 control=engines['control'];current=engines['current'];kuzu=engines['kuzu']
 gain=None if control.get('status') or current.get('status') else 1-current['median_process_p50_ms']/control['median_process_p50_ms']
 spread=None if gain is None else control['max_process_p50_ms']/control['min_process_p50_ms']-1
 primary=workload in ['label_scan_filter','all_nodes_property_filter','label_multi_property_filter','aggregation_group_by','aggregation_count_by_group','topk_property_sort']
 uncertain=False if gain is None else spread>.05 or abs(gain-(.10 if primary else -.05))<=.05
 rows.append({'scale':scale,'profile':profile,'workload':workload,'engines':engines,'current_p50_reduction_vs_control':gain,'control_p50_range_fraction':spread,'current_over_kuzu_p50':None if current.get('status') or kuzu.get('status') else current['median_process_p50_ms']/kuzu['median_process_p50_ms'],'primary':primary,'requires_preregistered_targeted_sampling':uncertain,'p95':'ten-sample descriptive only; no tail protection acceptance'})
(root/'read-comparison.json').write_text(json.dumps({'aggregation':'median independent-process P50; never pool processes','cases':rows},indent=2)+'\n')
with (root/'read-comparison.csv').open('w') as stream:
 writer=csv.writer(stream);writer.writerow(['scale','profile','workload','control_p50_ms','current_p50_ms','kuzu_p50_ms','current_reduction_percent','current_over_kuzu','control_range_percent','targeted_sampling_required'])
 for row in rows:
  writer.writerow([row['scale'],row['profile'],row['workload'],*[row['engines'][v].get('median_process_p50_ms') for v in ['control','current','kuzu']],None if row['current_p50_reduction_vs_control'] is None else row['current_p50_reduction_vs_control']*100,row['current_over_kuzu_p50'],None if row['control_p50_range_fraction'] is None else row['control_p50_range_fraction']*100,row['requires_preregistered_targeted_sampling']])
print(json.dumps({'workloads':len(rows),'targeted_sampling_candidates':sum(row['requires_preregistered_targeted_sampling'] for row in rows)}))
