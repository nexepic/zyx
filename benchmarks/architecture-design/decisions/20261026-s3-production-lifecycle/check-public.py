"""Untimed full-oracle public lifecycle checks on copies of admitted images."""
from pathlib import Path
import hashlib
import json
import subprocess
import sys
REPO=Path('/workspace/zyx');HERE=Path(__file__).resolve().parent
sys.path.insert(0,str(HERE.parent/'20261025-final-scan-comparison'))
import campaign
OUT=campaign.INPUT_ROOT/'s3-lifecycle'
CASES=['label_scan_filter','all_nodes_property_filter','aggregation_count_by_group','multi_key_group']
def sha(path):
 with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
for scale in ['medium','holdout']:
 source=campaign.INPUT_ROOT/f'{scale}.scan-corrected-v1.zyx';before=sha(source)
 for family,cases,mode in [('primary',CASES,'check-read')]+[(case,[case],'check-write') for case in ['property_set','edge_batch_1000','property_noop','edge_create_delete']]:
  stem=OUT/f'public-{scale}-{family}';target=stem.with_suffix('.db');assert not target.exists() and not stem.with_suffix('.json').exists()
  command=[str(OUT/'values-b'),mode,str(source),str(target),','.join(cases),'0','1'];print('START untimed',scale,family,flush=True)
  with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
   result=subprocess.run(command,env=campaign.ENV,stdout=out,stderr=err,preexec_fn=campaign.limits,timeout=1200)
  assert result.returncode==0,stem
  events=[json.loads(line) for line in stem.with_suffix('.jsonl').read_text().splitlines() if line.startswith('{')];assert len(events)==len(cases)
  for event in events:
   assert 'latency_ms' not in event and event.get('status')!='failed';campaign.check_event(event['case'],event,scale,mode=='check-write')
  assert sha(source)==before
  receipt={'status':'passed','query_benchmark':False,'full_oracle':True,'source_sha256':before,'command':command}
  stem.with_suffix('.json').write_text(json.dumps(receipt,indent=2)+'\n')
  for path in target.parent.glob(target.name+'*'):
   if path.is_file():path.unlink()
  print('FINISH untimed',scale,family,flush=True)
