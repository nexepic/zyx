from pathlib import Path
import subprocess,resource,json,hashlib
r=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume')
binary=r/'r43-ordinary-row-result-contract'
def limits():
    resource.setrlimit(resource.RLIMIT_AS,(6*1024**3,6*1024**3))
    resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024**3,2*1024**3))
report={'timing_benchmark':False,'purpose':'Untimed correctness preparation after verified ordinary adjacency image implementation','load_batch_rows':1000,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'cases':[]}
for scale in ['medium','holdout']:
    source=r/f'r7-inputs/{scale}';db=r/f'r7-inputs/{scale}-r43-ordinary.zyx'
    if db.exists(): raise RuntimeError('Refuse overwriting existing preparation input')
    with (r/f'r43-ordinary-{scale}-untimed-prepare.stdout').open('w') as out,(r/f'r43-ordinary-{scale}-untimed-prepare.stderr').open('w') as err:
        p=subprocess.run([str(binary),'prepare',str(source),str(db)],stdout=out,stderr=err,preexec_fn=limits,timeout=1800)
    case={'scale':scale,'returncode':p.returncode,'exists':db.exists()}
    if db.exists():case['bytes']=db.stat().st_size
    if p.returncode==0:case['sha256']=hashlib.sha256(db.read_bytes()).hexdigest()
    report['cases'].append(case);(r/'r43-ordinary-untimed-preparation.json').write_text(json.dumps(report,indent=2)+'\n')
    print(case,flush=True)
    if p.returncode:raise SystemExit(p.returncode)
