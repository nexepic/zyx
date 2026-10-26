from pathlib import Path
import sys,json,subprocess,hashlib,statistics
R=Path('/workspace/zyx'); D=Path(__file__).resolve().parent
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'))
import campaign
source=campaign.INPUT_ROOT/'medium.scan-corrected-v1.zyx'
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
identity={'input_sha256':sha(source),'binary_sha256':sha(D/'layer-probe')}
cmd=[str(D/'layer-probe'),str(source)]
with (D/'layer.jsonl').open('w') as out,(D/'layer.stderr').open('w') as err:
 p=subprocess.run(cmd,stdout=out,stderr=err,preexec_fn=campaign.limits,timeout=1200)
assert p.returncode==0,p.returncode
rows=[json.loads(l) for l in (D/'layer.jsonl').read_text().splitlines()]
assert len(rows)==10
oracle=campaign.data('medium')
for row in rows:
 assert row['count']==(oracle['numeric'] if row['case']=='all_nodes_property_filter' else oracle['countries']['CN']),row
 assert row['examined']==sum(oracle['counts'][k] for k in ('users','posts','tags')),row
assert sha(source)==identity['input_sha256']
summary={case:{key:statistics.median(r[key] for r in rows if r['case']==case) for key in rows[0] if key not in ('case','iteration')} for case in set(r['case'] for r in rows)}
(D/'layer-receipt.json').write_text(json.dumps({'identity':identity,'command':cmd,'oracle':'passed','input_unchanged':True,'formal_performance':False,'summary':summary},indent=2)+'\n')
print(json.dumps(summary,indent=2))
