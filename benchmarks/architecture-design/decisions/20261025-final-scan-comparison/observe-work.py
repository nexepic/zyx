"""Untimed planned work observations after all registered primary samples."""
from pathlib import Path
import hashlib,json,subprocess
import campaign
root=campaign.ROOT
freeze=json.loads((root/'freeze.json').read_text());identity=hashlib.sha256((root/'freeze.json').read_bytes()).hexdigest()
cases=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
for scale in ['medium','holdout']:
 for engine,tag in [('A','a'),('B','b')]:
  stem=campaign.OUT/f'values-{scale}-{engine}';target=stem.with_suffix('.db');assert not target.exists()
  source=Path(freeze['artifacts'][scale+('_scan' if engine=='B' else '_row')]['path'])
  command=[f'/tmp/zyx-final-{tag}-values','check-read',str(source),str(target),','.join(cases),'0','1']
  print('START untimed work',scale,engine,flush=True)
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
   result=subprocess.run(command,env=campaign.ENV,stdout=out,stderr=err,preexec_fn=campaign.limits,timeout=1200)
  assert result.returncode==0,stem
  events=[json.loads(line) for line in stem.with_suffix('.jsonl').read_text().splitlines() if line.startswith('{')]
  assert len(events)==4 and sorted(e['case'] for e in events)==sorted(cases),stem
  for event in events:
   assert event.get('status')!='failed' and 'latency_ms' not in event
   campaign.check_event(event['case'],event,scale)
  assert not target.exists(),target
  receipt={'freeze_sha256':identity,'status':'passed','query_benchmark':False,'scale':scale,'engine':engine,'cases':cases,'command':command,'purpose':'diagnose failed S2 primary gate; full S5 guard matrix remains incomplete'}
  stem.with_suffix('.json').write_text(json.dumps(receipt,indent=2)+'\n')
  print('FINISH untimed work',scale,engine,flush=True)
