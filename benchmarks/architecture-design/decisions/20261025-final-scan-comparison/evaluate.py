"""Evaluate every case without pooling datasets, engines or failed samples."""
from pathlib import Path
import csv,json
ROOT=Path('/workspace/zyx/benchmarks/architecture-design/results/20261024-work-scan/final-comparison')
rows={(r['scale'],r['case'],r['engine']):r for r in csv.DictReader((ROOT/'summary.csv').open())}
primary=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
report={'primary':[],'guards':[],'acceptance':'pending'}
for scale in ['medium','holdout']:
 for case in primary:
  for candidate in ['A','B']:
   row=rows.get((scale,case,candidate));controls=[rows.get((scale,case,c)) for c in ['initial','control']];k=rows.get((scale,case,'kuzu'))
   valid=bool(row and k and all(controls) and int(k['samples'])==30 and int(row['samples'])==30 and all(int(c['samples'])==30 for c in controls))
   speedups={name:float(c['p50_ms'])/float(row['p50_ms']) for name,c in zip(['initial','control'],controls)} if valid else {}
   report['primary'].append({'scale':scale,'case':case,'candidate':candidate,'valid':valid,'speedup':speedups,'passes_5x_each_control':valid and all(v>=5 for v in speedups.values()),'candidate_p50_ms':float(row['p50_ms']) if row else None,'kuzu_p50_ms':float(k['p50_ms']) if k else None,'beats_kuzu':valid and float(row['p50_ms'])<float(k['p50_ms'])})
expected_guards=[(scale,case,engine) for scale in ['medium','holdout'] for case in ['point_lookup_indexed','graph_full_point','graph_full_neighbors','one_hop_expand','two_hop_expand','property_set','edge_batch_1000','property_noop','edge_create_delete'] for engine in ['A','B']]
expected_guards += [(scale,case,'A') for scale,cases in [('large',['large_selected','large_full','large_patch']),('skew',['skew_out','skew_both','skew_twohop'])] for case in cases]
for scale,case,engine in expected_guards:
 r=rows.get((scale,case,engine));controls=[rows.get((scale,case,c)) for c in ['initial','control']]
 valid=bool(r and int(r['samples'])==30 and all(c and int(c['samples'])==30 for c in controls))
 ratios={name:{metric:float(r[metric])/float(c[metric]) for metric in ['p50_ms','p95_ms']} for name,c in zip(['initial','control'],controls)} if valid else {}
 report['guards'].append({'scale':scale,'case':case,'candidate':engine,'valid':valid,'ratios':ratios,'confirmation_required':not valid or any(v>1.05 for metrics in ratios.values() for v in metrics.values())})
completion=json.loads((ROOT/'completion.json').read_text()) if (ROOT/'completion.json').exists() else {}
report['measurement_complete']=bool(completion.get('all_workers_passed') and completion.get('processes')==177 and completion.get('measured_samples')==3900)
report['completion']=completion
report['failed_workers']=[str(path.relative_to(ROOT)) for path in sorted((ROOT/'workers').glob('*.json')) if json.loads(path.read_text()).get('status')=='failed']
report['acceptance']='measurement requires separate correctness, lifecycle, space and stable-guard assessment; never automatic 100%'
(ROOT/'gate-evaluation.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(report,indent=2))
