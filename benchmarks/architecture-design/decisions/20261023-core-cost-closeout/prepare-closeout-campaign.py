from pathlib import Path
import shutil,json,hashlib,subprocess
r=Path('/workspace/zyx');old=r/'benchmarks/architecture-design/decisions/20261022-core-cost-final';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-closeout';d.mkdir(exist_ok=True)
o=r/'benchmarks/architecture-design/results/20261022-core-cost/closeout'
for n in ['campaign.py','preflight.py','oracle.py','registration.json','public-probe.cpp','import-probe.cpp','kuzu-worker.py','kuzu-import.py','kuzu-reopened-import.py']:
 s=(old/n).read_text()
 if n in ['campaign.py','preflight.py']:s=s.replace("INPUT_ROOT/'final'","INPUT_ROOT/'closeout'").replace("20261022-core-cost/final'","20261022-core-cost/closeout'")
 (d/n).write_text(s)
# Preserve the frozen measurement contract; no threshold/sample/dataset changes.
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
if not (o/'inputs').exists(): (o/'inputs').symlink_to('../final/inputs',target_is_directory=True)
for scale in ['medium','holdout']:
 name=scale+'-summary-init.json';receipt=json.loads((r/'benchmarks/architecture-design/results/20261022-core-cost/final'/name).read_text())
 assert sha(o/'inputs'/(scale+'-summary.zyx'))==receipt['target_sha256']
 receipt['closeout_reuse']='Unchanged immutable preinitialized source; initialization excluded and old initialization cost/identity retained, not rerun.'
 (d/('final-'+name)).write_text(json.dumps(receipt,indent=2)+'\n')
files=subprocess.check_output(['git','diff','--name-only'],cwd=r,text=True).splitlines()+subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','cmake','tests'],cwd=r,text=True).splitlines()
files=sorted(set(f for f in files if f.startswith(('include/','src/','cmake/','tests/'))))
f={'base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'release_library_sha256':sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a')),'source_files':{n:sha(r/n) for n in files},'formal_build':json.loads((o/'build.json').read_text()),'experimental_adoption':False,'implementation_progress_percent':90,'P7_accepted':False}
(d/'freeze.json').write_text(json.dumps(f,indent=2)+'\n')
shutil.copyfile('/tmp/build-closeout-final-probes.py',d/'build-final-probes.py')
print(d)
