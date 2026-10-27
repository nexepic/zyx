"""R0 low-frequency leaf CPU samples with paired observation controls."""
from pathlib import Path
import json,sys,statistics,hashlib,subprocess
from collections import defaultdict,Counter
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'current-cpu';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
assert not (O/'protocol.json').exists()
protocol={'cases':['all_nodes_property_filter','label_scan_filter','multi_key_group'],'iterations':100,'warmups':4,'interval_ms':10,'quality_max_perturbation':.05,'scope':'Leaf samples only, no stacks or hardware counters; periodic sampling misses short initial work; percentages are sample shares, not exact CPU costs.','library_sha256':sha(R/'build-scan-candidate/libzyx_core.a'),'input_sha256':sha(B/'medium.zyx'),'probe_sha256':sha(O/'probe'),'minimum_independent_samples':100}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');records=[]
for case in protocol['cases']:
 stem=O/case;cmd=['/usr/bin/taskset','-c','0',str(O/'probe'),'read',str(B/'medium.zyx'),'/tmp/zyx-current-cpu-'+case+'.db',case,'4','100'];print('START',case,flush=True)
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:r=run(cmd,out,err,dict(campaign.ENV,ZYX_CPU_SAMPLE_ENABLE='1',ZYX_CPU_SAMPLE_OUTPUT=str(stem.with_suffix('.pcs'))),campaign.REG['limits'])
 stem.with_suffix('.resources.json').write_text(json.dumps(r,indent=2)+'\n');assert r['returncode']==0,r
 rows=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(rows)==100
 for x in rows:campaign.check_event(case,x,'medium',False)
 a=[x for x in rows if x['sampled']];b=[x for x in rows if not x['sampled']];assert len(a)==len(b)==50
 ratio=statistics.median(x['latency_ms'] for x in a)/statistics.median(x['latency_ms'] for x in b)
 groups=defaultdict(list);lines=stem.with_suffix('.pcs').read_text().splitlines()
 for l in lines[1:]:
  module,address,weight,phase=l.split('\t');groups[module].append((address,int(weight)))
 leaves=Counter();weighted=Counter()
 for module,entries in groups.items():
  if Path(module).is_file():
   output=subprocess.check_output(['/workspace/zyx-sysroot/usr/bin/llvm-symbolizer-19','--obj='+module,'--output-style=JSON','--no-inlines'],input='\n'.join('0x'+x[0] for x in entries)+'\n',text=True)
   values=[]
   for line in output.splitlines():
    if line.strip():
     v=json.loads(line);values.extend(v if isinstance(v,list) else [v])
   assert len(values)==len(entries)
   names=[(x.get('Symbol') or [{}])[0].get('FunctionName','') for x in values]
  else:names=['']*len(entries)
  for name,(_,weight) in zip(names,entries):
   name=name if name and name!='??' else 'unresolved '+module;leaves[name]+=1;weighted[name]+=weight
 record={'case':case,'command':cmd,'oracles':100,'sampled_control_median_ratio':ratio,'independent_samples':sum(leaves.values()),'quality_passed':abs(ratio-1)<=.05 and sum(leaves.values())>=100,'control_p50_ms':statistics.median(x['latency_ms'] for x in b),'observed_p50_ms':statistics.median(x['latency_ms'] for x in a),'cpu_wall_ratio':statistics.median((x['user_ms']+x['system_ms'])/x['latency_ms'] for x in a),'major_faults':sum(x['major_faults'] for x in rows),'involuntary_switches':sum(x['involuntary_switches'] for x in rows),'leaves':leaves.most_common(),'overrun_weighted_leaves':weighted.most_common()}
 stem.with_suffix('.receipt.json').write_text(json.dumps(record,indent=2)+'\n');records.append(record);print('PASS',case,'quality',record['quality_passed'],'ratio',ratio,'samples',record['independent_samples'],'TOP',record['leaves'][:5],flush=True)
assert sha(B/'medium.zyx')==protocol['input_sha256'];assert sha(R/'build-scan-candidate/libzyx_core.a')==protocol['library_sha256']
(O/'summary.json').write_text(json.dumps(records,indent=2)+'\n');print('COMPLETE300 full result oracles',flush=True)
