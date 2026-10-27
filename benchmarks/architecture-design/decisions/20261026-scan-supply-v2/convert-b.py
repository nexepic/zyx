"""One immutable source to a fresh field-cohort candidate; not a query benchmark."""
from pathlib import Path
import hashlib,json,sys,time
from process_receipt import run
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent;O=D/'b-field-cohort'
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
name=sys.argv[1];assert name in ['medium','holdout','generality']
freeze=json.loads((O/'source-freeze.json').read_text());info=freeze['conversion_sources'][name];source=Path(info['path']);target=O/(name+'.zyx');assert not target.exists()
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
assert sha(source)==info['sha256']
assert sha(R/'build-scan-candidate/zyx-scan-convert')==freeze['converter_binary_sha256']
cmd=[str(R/'build-scan-candidate/zyx-scan-convert'),str(source),str(target),'--field-cohort','8'];start=time.monotonic()
(O/(name+'-conversion-start.json')).write_text(json.dumps(dict(command=cmd,source=info,cohort_groups=8,timing_benchmark=False,limits=campaign.REG['limits']),indent=2)+'\n')
with (O/(name+'-conversion.stdout')).open('w') as out,(O/(name+'-conversion.stderr')).open('w') as err:
 resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'],timeout=7200)
receipt=dict(command=cmd,resources=resources,conversion_wall_seconds=time.monotonic()-start,source_sha256_before=info['sha256'],source_sha256_after=sha(source),target_bytes=target.stat().st_size if target.exists() else None,target_sha256=sha(target) if resources['returncode']==0 else None,converter_sha256=freeze['converter_binary_sha256'],library_sha256=freeze['library_sha256'],query_benchmark=False,measurement_scope='conversion; may overlap other preparation, no latency comparison claimed')
(O/(name+'-conversion-receipt.json')).write_text(json.dumps(receipt,indent=2)+'\n')
assert resources['returncode']==0 and receipt['source_sha256_before']==receipt['source_sha256_after']
print(name,'CONVERSION COMPLETE',flush=True)
