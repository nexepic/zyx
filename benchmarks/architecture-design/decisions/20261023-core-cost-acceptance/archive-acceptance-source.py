from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,tarfile,shutil
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-acceptance';sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
freeze=json.loads((d/'freeze.json').read_text())
for name,value in freeze['source_files'].items():assert sha(r/name)==value,name
assert sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a'))==freeze['release_library_sha256']
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
new=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as t:
 for name in new:t.add(r/name,arcname=name)
for name in ['update-final-acceptance-plan.py','clean-final-acceptance-specs.py','report-acceptance-performance.py','finalize-acceptance-report.py','archive-acceptance-source.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
input_paths=set()
for p in (r/'benchmarks/architecture-design/results/20261022-core-cost/acceptance/workers').glob('*.json'):
 record=json.loads(p.read_text());cmd=record['command']
 for arg in cmd:
  path=Path(arg)
  if path.is_file() and path.suffix in ['.zyx','.kuzu']:input_paths.add(path)
identities={str(p.relative_to(r)): {'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(input_paths)}
(d/'input-identities.json').write_text(json.dumps(identities,indent=2)+'\n')
files=[p for p in d.iterdir() if p.is_file() and p.name!='artifact-sha256.json']
(d/'artifact-sha256.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'base_head':freeze['base_head'],'source_patch_sha256':sha(d/'source.patch'),'additional_source_sha256':sha(d/'additional-source.tar.gz'),'files':{p.name:sha(p) for p in sorted(files)}},indent=2)+'\n')
print('source and input identities archived',len(identities),'input databases')
