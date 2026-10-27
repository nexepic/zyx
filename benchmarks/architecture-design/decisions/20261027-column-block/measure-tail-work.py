"""R0 targeted per-query work-count correlation, not a performance acceptance window."""
from pathlib import Path
import json,sys,hashlib,statistics
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;O=N/'tail-work';D=N.parent/'20261026-scan-supply-v2';B=D/'b-field-cohort'
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
source=B/'medium.zyx';core=R/'build-scan-candidate/libzyx_core.a';assert not (O/'protocol.json').exists()
keys=['direct_rows','group_decodes','membership_decodes','column_decodes','file_size_calls','buffer_hits','buffer_misses','checksum_bytes','aggregate_input_rows']
protocol={'cases':['all_nodes_property_filter','multi_key_group'],'iterations':40,'warmups':4,'source_sha256':sha(source),'core_sha256':sha(core),'probe_sha256':sha(O/'probe'),'keys':keys,'scope':'Selected synchronous work counters and per-query CPU/wall. No acceptance timing; observer is active on every query, no isolated perturbation gate; correlations cannot infer causes for old formal window. No hardware counters.'}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');results=[]
for case in protocol['cases']:
 stem=O/case;cmd=['/usr/bin/taskset','-c','0',str(O/'probe'),'read',str(source),'/tmp/zyx-tail-work-'+case+'.db',case,'4','40']
 print('START',case,flush=True)
 with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:r=run(cmd,out,err,dict(campaign.ENV),campaign.REG['limits'])
 stem.with_suffix('.resources.json').write_text(json.dumps(r,indent=2)+'\n');assert r['returncode']==0,r
 rows=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];counts=[json.loads(l) for l in stem.with_suffix('.stderr').read_text().splitlines() if l.startswith('{"iteration"')]
 assert len(rows)==len(counts)==40
 for row in rows:campaign.check_event(case,row,'medium',False)
 indexed={x['iteration']:x['counts'] for x in counts}; assert len(indexed)==40
 med=statistics.median(x['latency_ms'] for x in rows);tails=[x for x in rows if x['latency_ms']>2*med]; normal=[x for x in rows if x['latency_ms']<=2*med]
 summary={'case':case,'oracles':40,'p50_ms':med,'slow_count_over_2x_p50':len(tails),'slow_samples':[{'iteration':x['iteration'],'latency_ms':x['latency_ms'],'cpu_ms':x['user_ms']+x['system_ms'],'major_faults':x['major_faults'],'involuntary_switches':x['involuntary_switches'],'counts':dict(zip(keys,indexed[x['iteration']]))} for x in tails], 'normal_counter_ranges':{k:[min(indexed[x['iteration']][i] for x in normal),max(indexed[x['iteration']][i] for x in normal)] for i,k in enumerate(keys)},'all_counter_ranges':{k:[min(v[i] for v in indexed.values()),max(v[i] for v in indexed.values())] for i,k in enumerate(keys)}}
 (O/(case+'.receipt.json')).write_text(json.dumps(summary,indent=2)+'\n');results.append(summary);print('DONE',case,'slow',len(tails),flush=True)
assert sha(source)==protocol['source_sha256'] and sha(core)==protocol['core_sha256']
(O/'summary.json').write_text(json.dumps(results,indent=2)+'\n')
