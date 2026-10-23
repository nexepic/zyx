from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-multi-cold';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['multi-cold-legacy-','multi-cold-compact-'],492,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],153,'zyx-build-core-candidate')]:
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('multi-cold-',16,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
for name in ['add-multi-cold-blocks.py','test-multi-cold-blocks.py','update-multi-cold-docs.py','record-multi-cold.py','merge-cold-block-hot.py','add-cold-hot-merge-tests.py','update-cold-hot-merge-docs.py','record-cold-hot-merge.py','add-cold-block-preparation.py','add-cold-block-preparation-tests.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','update-cold-block-preparation-docs.py','record-cold-block-preparation.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'implemented':['One-group multiple affected cold blocks with combined64hot/cold change bound','Preflight distinct blocks/owner roots, original-pin source authentication and accumulated private-image updates','Continue after group/membership relocation, retain one final GroupDirectory upsert','Preserve preceding hot value/presence/NULL and cold changes; discard whole batch on later failure'],'remaining':['New cold block creation and same-batch LargeValues','Actual Native field dispatch/schema/constraint/index coordination','Global hot demotion, owner liveness/delete/inverse/public atomic lifecycle','Whole-space admission and final P7 performance matrix']},indent=2)+'\n')
(d/'README.md').write_text("""# Multiple affected cold blocks in the original batch

Progress90%, P7 open. No full suite/timing/commit/push. Original format/quotas/bounds remain. Source/reports/recipes/current library fingerprints retained without duplicating binaries.

`prepareColdBlocks` preflights one group, distinct source blocks/owner roots and combined64hot/cold changes. Original source pin/GroupDirectory/membership authenticate each source, while updates use the current accumulated private images. After group/membership relocation, later blocks follow its private directory locator; intermediate moves replace one final group upsert. Already changed source owners cannot reenter. Existing hot presence/value/NULL and earlier cold changes remain. Failure discards the whole existing batch with no independent ID/file/root publication.

16 targeted cases pass, including63-owner split/relocation followed by updating the second cold block and combining hot NULL insertion with clearing two cold blocks; duplicates reject. Both experimental memberships pass492 related module cases; default153 pass, no failures/skips. Private preparation does not establish public-conversion lifecycle/performance acceptance. New cold-block creation/batch-local values, actual Native field/schema/index/constraint dispatch, global demotion/owner lifecycle/inverse/publication, whole-space admission and final P7 remain pending.
""")
print(checks)
