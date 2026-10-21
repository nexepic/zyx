from pathlib import Path
import json,subprocess,resource,shutil,csv,hashlib
r=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume')
def caps():
 resource.setrlimit(resource.RLIMIT_AS,(6*1024**3,6*1024**3));resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024**3,2*1024**3))
report={'timing_benchmark':False,'binary_sha256':hashlib.sha256((r/'r7-final-row-result-contract').read_bytes()).hexdigest(),'oracle_correction':'DDL returns the documented scalar Index created, not zero rows. Preserve the initial fixture failure; do not repeat already-successful DDL.','cases':[]}
initial=r/'r7-final-indexed-initial-ddl-oracle-error.json'
if not initial.exists():shutil.copyfile(r/'r7-final-indexed-untimed-correctness.json',initial)
for scale in ['medium','holdout']:
 target=r/f'r7-inputs/{scale}-r7-final-indexed.zyx'
 if scale=='medium':assert target.exists()
 else:
  assert not target.exists();shutil.copyfile(r/f'r7-inputs/{scale}-r43-identity.zyx',target)
 users=list(csv.DictReader((r/f'r7-inputs/{scale}/users.csv').open()))
 checks=[('create_country','CREATE INDEX FOR (u:User) ON (u.country)',[[['string','Index created']]]),('create_age','CREATE INDEX FOR (u:User) ON (u.age)',[[['string','Index created']]]),('country_count',"MATCH (u:User) WHERE u.country='CN' RETURN count(u)",[[['integer',sum(u['country']=='CN' for u in users)]]]),('age_count','MATCH (u:User) WHERE u.age>=30 RETURN count(u)',[[['integer',sum(int(u['age'])>=30 for u in users)]]]),('multi_count',"MATCH (u:User) WHERE u.country='CN' AND u.age>=30 RETURN count(u)",[[['integer',sum(u['country']=='CN' and int(u['age'])>=30 for u in users)]]]),('country_rows',"MATCH (u:User) WHERE u.country='CN' RETURN u.id ORDER BY u.id LIMIT 10",[[['string',u['id']]] for u in sorted([u for u in users if u['country']=='CN'],key=lambda u:u['id'])[:10]])]
 for name,query,expected in checks:
  path=r/f'r7-final-indexed-{scale}-{name}'
  if scale=='medium' and name=='create_country':code=0
  else:
   with Path(str(path)+'.json').open('w') as out,Path(str(path)+'.stderr').open('w') as err:
    p=subprocess.run([str(r/'r7-final-row-result-contract'),'query',str(target),query],stdout=out,stderr=err,preexec_fn=caps,timeout=900)
   code=p.returncode
  actual=json.loads(Path(str(path)+'.json').read_text()) if code==0 else None
  c={'scale':scale,'name':name,'query':query,'returncode':code,'passed':code==0 and actual==expected,'expected':expected};report['cases'].append(c);(r/'r7-final-indexed-untimed-correctness.json').write_text(json.dumps(report,indent=2)+'\n');print(scale,name,c['passed'],flush=True)
  if not c['passed']:raise SystemExit(1)
 report.setdefault('inputs',{})[scale]={'bytes':target.stat().st_size,'sha256':hashlib.file_digest(target.open('rb'),'sha256').hexdigest()};(r/'r7-final-indexed-untimed-correctness.json').write_text(json.dumps(report,indent=2)+'\n')
