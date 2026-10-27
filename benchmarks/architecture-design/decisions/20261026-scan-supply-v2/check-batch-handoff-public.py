"""Untimed public work and generic lifecycle verification of batch handoff."""
from pathlib import Path
import hashlib,json,subprocess,shutil,os,sys,time
D=Path(__file__).resolve().parent;R=Path('/workspace/zyx');B=D/'batch-handoff-probes'
assert json.loads((D/'batch-handoff-scan/receipt.json').read_text())['failures']==0
assert not B.exists();B.mkdir()
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
lib=R/'build-scan-candidate/libzyx_core.a'
assert sha(lib)==json.loads((D/'batch-handoff-scan/receipt.json').read_text())['library_sha256']
for name in ['public','values','allocation','generality']:
 cmd=json.loads((D/'record-view-probes'/(name+'-link.json')).read_text())
 cmd=[x.replace(str(D/'record-view-probes'),str(B)) for x in cmd]
 (B/(name+'-link.json')).write_text(json.dumps(cmd,indent=2)+'\n')
 with (B/(name+'-link.log')).open('w') as out:subprocess.run(cmd,stdout=out,stderr=out,check=True)
source=R/'benchmarks/architecture-design/results/20261024-work-scan/generality.scan-ready.zyx'
before=sha(source);assert before=='150ae1938505e31d692a5d71596d194b1f9f8baebeaf55ade1e92562f0f01b43'
target=B/'generality-private.zyx';shutil.copyfile(source,target)
cmd=[str(B/'generality'),'verify',str(target)];start=time.monotonic()
with (B/'generality.log').open('w') as out:result=subprocess.run(cmd,stdout=out,stderr=out,timeout=1200)
receipt=dict(command=cmd,returncode=result.returncode,source_sha256_before=before,source_sha256_after=sha(source),binary_sha256=sha(B/'generality'),library_sha256=sha(lib),default_query_timeout_ms=30000,owners=16384,mixed_mutation_batches=512,wall_seconds=time.monotonic()-start,target_bytes=target.stat().st_size,timing_benchmark=False)
(B/'generality-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');assert result.returncode==0 and sha(source)==before
subprocess.run([sys.executable,str(D/'check-batch-handoff-work.py')],check=True)
