"""Recover summary after all28 workers passed; do not run any measurement."""
from pathlib import Path
import json,hashlib,statistics,sys
D=Path(__file__).resolve().parent;B=D/'b-field-cohort';O=B/'guard-review-1';R=D.parents[3]
freeze=json.loads((B/'final-primary/freeze.json').read_text())
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
for k,v in freeze['zyx_binaries'].items():assert sha(v['path'])==v['sha256']
for p,h in freeze['source_hashes'].items():assert sha(R/p)==h
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
for scale in ('medium','holdout'):
 for p in (campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx'),B/(scale+'.zyx')):assert sha(p)==freeze[str(p)]
receipts=sorted(O.glob('*.receipt.json'));assert len(receipts)==28
rows=[];opened=[]
for p in receipts:
 receipt=json.loads(p.read_text());assert receipt['oracle']=='passed' and receipt['resources']['returncode']==0
 rnd,scale,mode,rest=p.name.split('-',3);engine=rest.split('.')[0].rsplit('-',1)[1]
 for line in p.with_name(p.name.replace('.receipt.json','.jsonl')).read_text().splitlines():
  if not line.startswith('{'):continue
  x=json.loads(line)
  if 'case' in x and 'rows' in x:
   campaign.check_event(x['case'],x,scale,mode=='write')
   if mode=='read' and '-read-opened-' in p.name:continue
   rows.append(dict(x,engine=engine,scale=scale,round=int(rnd)))
  if x.get('event')=='handle_lifecycle':opened.append(dict(x,engine=engine,scale=scale,round=int(rnd)))
def stat(xs):
 xs=sorted(xs);q=.95*(len(xs)-1);i=int(q)
 return dict(p50_ms=statistics.median(xs),p95_ms=xs[i]+(xs[min(i+1,len(xs)-1)]-xs[i])*(q-i),samples=len(xs))
summary=[]
for scale in ('medium','holdout'):
 for case in sorted(set(x['case'] for x in rows if x['scale']==scale)):
  record=dict(scale=scale,case=case)
  for engine in ('control','candidate'):
   selected=[x for x in rows if x['scale']==scale and x['case']==case and x['engine']==engine];assert len(selected)==20
   record[engine]=stat([x['latency_ms'] for x in selected]);record[engine]['process_p50_ms']=[statistics.median(x['latency_ms'] for x in selected if x['round']==rnd) for rnd in range(2)]
  for q in ('p50','p95'):record['candidate_over_control_'+q]=record['candidate'][q+'_ms']/record['control'][q+'_ms']
  record['review_required']=max(record['candidate_over_control_p50'],record['candidate_over_control_p95'])>1.05;summary.append(record)
assert len(rows)==360 and len(opened)==8
(O/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(O/'open-close.json').write_text(json.dumps({'scope':'Direct open/close clock intervals, two additional observations per engine/dataset','samples':opened},indent=2)+'\n')
(O/'summary-recovery.json').write_text(json.dumps({'measurement_workers':28,'all_worker_returncodes':0,'guard_oracles':360,'open_oracles':8,'problem':'Original summary enumerated global case set and requested holdout property_noop, which was intentionally not measured. Recovery groups cases by scale. No measurements rerun.','source_and_inputs_reverified':True},indent=2)+'\n')
print('PASS360 guard oracles plus8 opens; hashes reverified; offline summary recovered')
