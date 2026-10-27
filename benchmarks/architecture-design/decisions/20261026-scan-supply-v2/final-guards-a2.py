"""Remaining guard/lifecycle observations in the same final window; no primary reruns."""
from pathlib import Path
import hashlib,json,statistics,subprocess,sys
from process_receipt import run
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;O=D/'final-guards-a2'
assert '--validated-run' in sys.argv
assert (D/'final-primary-a2/summary.json').exists(), 'Primary window must finish first'
assert json.loads((D/'final-primary-a2/gates.json').read_text())['gain_gate_passed'], 'Do not run full guards for a failed primary candidate'
assert not O.exists(), 'Preserve measurements; retest only identified failures'
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
freeze=json.loads((D/'final-primary-a2/freeze.json').read_text())
sha=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
binaries={k:Path(v['path']) for k,v in freeze['zyx_binaries'].items()}
for k,p in binaries.items():assert sha(p)==freeze['zyx_binaries'][k]['sha256']
for p,h in freeze['source_hashes'].items():assert sha(R/p)==h
O.mkdir();(O/'protocol.json').write_text(json.dumps({'measurement_script_sha256':sha(__file__),'primary_freeze_sha256':sha(D/'final-primary-a2/freeze.json'),'processes':3,'warmups':3,'iterations':10,'read_guards':['point_lookup_indexed','graph_full_point','one_hop_fetch_neighbor_records'],'write_guards':['property_set','property_noop','edge_batch_1000'],'open_observation_samples_per_engine_dataset':3,'query_benchmark':True},indent=2)+'\n')
rows=[];opened=[]
def worker(engine,scale,mode,cases,rnd,warm=3,iterations=10):
    stem=O/f'{rnd}-{scale}-{mode}-{cases[0]}-{engine}'
    target=Path('/tmp')/('zyx-v4-a2-guard-'+stem.name+'.db');assert not target.exists()
    source=campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx')
    cmd=[str(binaries[engine]),mode,str(source),str(target),','.join(cases),str(warm),str(iterations)]
    print('START',stem.name,flush=True)
    with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'])
    events=[json.loads(x) for x in stem.with_suffix('.jsonl').read_text().splitlines() if x.startswith('{')]
    receipt={'command':cmd,'resources':resources,'oracle':'pending'}
    stem.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    assert resources['returncode']==0,receipt
    queries=[x for x in events if 'case' in x and 'rows' in x]
    assert len(queries)==len(cases)*iterations,(stem,len(queries))
    for x in queries:
        campaign.check_event(x['case'],x,scale,mode=='write')
        if mode!='read-opened':rows.append(dict(x,engine=engine,scale=scale,round=rnd))
    for x in events:
        if x.get('event')=='handle_lifecycle':opened.append(dict(x,engine=engine,scale=scale,round=rnd))
    receipt['oracle']='passed';receipt['samples']=len(queries)
    stem.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS',stem.name,flush=True)
for rnd in range(3):
 for scale in ['medium','holdout']:
  for engine in (['control','candidate'] if rnd%2==0 else ['candidate','control']):
   worker(engine,scale,'read',['point_lookup_indexed','graph_full_point','one_hop_fetch_neighbor_records'],rnd)
   for case in ['property_set','property_noop','edge_batch_1000']:worker(engine,scale,'write',[case],rnd)
   worker(engine,scale,'read-opened',['all_nodes_property_filter'],rnd,0,1)
def stat(xs):
    xs=sorted(xs);q=.95*(len(xs)-1);i=int(q)
    return {'p50_ms':statistics.median(xs),'p95_ms':xs[i]+(xs[min(i+1,len(xs)-1)]-xs[i])*(q-i),'samples':len(xs)}
summary=[]
for scale in ['medium','holdout']:
 for case in sorted(set(x['case'] for x in rows)):
  record={'scale':scale,'case':case}
  for engine in binaries:
   record[engine]=stat([x['latency_ms'] for x in rows if x['scale']==scale and x['case']==case and x['engine']==engine])
   record[engine]['process_p50_ms']=[statistics.median(x['latency_ms'] for x in rows if x['scale']==scale and x['case']==case and x['engine']==engine and x['round']==rnd) for rnd in range(3)]
  record['candidate_over_control_p50']=record['candidate']['p50_ms']/record['control']['p50_ms']
  record['candidate_over_control_p95']=record['candidate']['p95_ms']/record['control']['p95_ms']
  record['review_required']=max(record['candidate_over_control_p50'],record['candidate_over_control_p95'])>1.05
  summary.append(record)
(O/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(O/'open-close.json').write_text(json.dumps({'scope':'direct open/close clock intervals; one observation per independent worker, three per engine/dataset; not process wall subtraction','samples':opened},indent=2)+'\n')
for k,p in binaries.items():assert sha(p)==freeze['zyx_binaries'][k]['sha256']
for p,h in freeze['source_hashes'].items():assert sha(R/p)==h
for scale in ['medium','holdout']:
 p=campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx');assert sha(p)==freeze[str(p)]
print('COMPLETE',len(rows),'guard oracle samples;',len(opened),'direct handle observations',flush=True)
