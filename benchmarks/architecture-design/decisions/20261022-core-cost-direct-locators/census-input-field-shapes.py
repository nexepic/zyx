from pathlib import Path
import csv,json,hashlib
root=Path('/workspace/zyx'); inputs=root/'benchmarks/architecture-design/results/20261020-work-resume/r7-inputs';out=root/'benchmarks/architecture-design/decisions/20261022-core-cost-direct-locators'
report={'timing_benchmark':False,'source':'apps/compare_benchmark/main.cpp','source_sha256':hashlib.sha256((root/'apps/compare_benchmark/main.cpp').read_bytes()).hexdigest(),'short_text_limit_bytes':7,'datasets':{}}
for dataset in ['medium','holdout']:
 tables={};total=0;long_id=0
 for name,text_fields in [('users',['id','country']),('posts',['id']),('tags',['id'])]:
  path=inputs/dataset/(name+'.csv');stats={key:{'count':0,'min_bytes':None,'max_bytes':0,'at_most_7_bytes':0,'above_7_bytes':0} for key in text_fields};n=0
  with path.open(newline='') as f:
   for row in csv.DictReader(f):
    n+=1
    for key,s in stats.items():
     length=len(row[key].encode('utf-8'));s['count']+=1;s['min_bytes']=length if s['min_bytes'] is None else min(s['min_bytes'],length);s['max_bytes']=max(s['max_bytes'],length);s['at_most_7_bytes']+=int(length<=7);s['above_7_bytes']+=int(length>7)
  total+=n;long_id+=stats['id']['above_7_bytes'];tables[name]={'rows':n,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'text_fields':stats}
 report['datasets'][dataset]={'tables':tables,'nodes':total,'nodes_with_long_id':long_id,'every_node_has_long_id':long_id==total,'dense_id_full_group_estimate':total//64,'dense_id_groups_including_tail_estimate':(total+63)//64,'group_estimate_note':'Consecutive-ID estimate only; actual persisted ID leases/gaps are not inspected.','pure_column_cohort_eligible':False if long_id==total else 'not_proven'}
(out/'input-field-shapes.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:{a:b for a,b in v.items() if a!='tables'} for k,v in report['datasets'].items()},indent=2))
