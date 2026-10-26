"""Untimed public-query work counters with complete existing result oracles."""
from pathlib import Path
import hashlib,json,subprocess,sys
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'))
import campaign
source=campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'
cases=['all_nodes_property_filter','label_scan_filter','aggregation_count_by_group','multi_key_group']
target=D/'medium-work-check.db'
assert not target.exists(),target
cmd=[str(D/'values-b'),'check-read',str(source),str(target),','.join(cases),'0','1']
def sha(path):
 with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
identity={'source_sha256':sha(source),'binary_sha256':sha(D/'values-b')}
with (D/'candidate-work.jsonl').open('w') as out,(D/'candidate-work.stderr').open('w') as err:
 p=subprocess.run(cmd,stdout=out,stderr=err,timeout=1200,preexec_fn=campaign.limits)
assert p.returncode==0,p.returncode
rows=[json.loads(x) for x in (D/'candidate-work.jsonl').read_text().splitlines()]
assert len(rows)==len(cases)
for case,row in zip(cases,rows):
 assert row['case']==case
 campaign.check_event(case,row,scale='medium')
assert sha(source)==identity['source_sha256']
(D/'work-receipt.json').write_text(json.dumps({'command':cmd,'identity':identity,'complete_oracles':len(rows),'input_unchanged':True,'formal_performance':False},indent=2)+'\n')
print('PASS: four complete public-query oracles; no timing benchmark')
