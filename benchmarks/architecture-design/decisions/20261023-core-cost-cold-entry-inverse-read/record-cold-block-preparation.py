from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-block-preparation';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-block-preparation-legacy-','cold-block-preparation-compact-'],489,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],153,'zyx-build-core-candidate')]:
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('initial-',10,3),('corrected-',10,0),('complete-',13,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
for name in ['add-cold-block-preparation.py','add-cold-block-preparation-tests.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','update-cold-block-preparation-docs.py','record-cold-block-preparation.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'format':'experimental13/39, default5/31; unchanged','implemented':['Existing DemotionPreparation single-block complete-owner-root private preparation','Original source pin/batch/GroupDirectory/Entity Map/competing ROW/legacy selector and actual manifest authentication','Hot-token exclusion and inline canonical-value validation','At most two original FieldPageStore COW images, typed cold overwrite clearing source proof, exact matrix/group updates','Last cold row erase with live/propertyless owners retained','No file/root/I/O epoch/physical or logical ID changes; unrelated cold corruption not decoded'],'initial_source_bug':'Target generation was incorrectly passed to published-snapshot inspector; original pin fence now authenticates existing LargeValues, without weakening checks/tests','remaining':['Block splitting, matrix growth relocation, GroupDirectory/cold-binding atomic updates','Merging hot/cold changes and same-batch new values','Token catalog/constraint/index and actual Native publication coordination','Owner liveness/demotion/delete/inverse and full public-conversion lifecycle','Whole-space admission and final P7 performance matrix']},indent=2)+'\n')
(d/'README.md').write_text('''# Existing preparer: actual-source single cold-block private updates

Accepted progress remains **90%**, P7 open. No full unit suite, timing benchmark, commit or push. Format remains experimental13/39/default5/31. Intermediate source/reports/recipes/current library fingerprints are preserved without duplicating binaries; prior key frozen pairs remain.

`HybridPropertyDemotionPreparation::prepareColdBlock` authenticates the original pin/batch, GroupDirectory, actual Entity Map, competing ordinary ROW and legacy selectors. Actual Group/matrix/cold block binding and affected owners are checked; unchanged blocks are not decoded. Original read-only LargeValueStore authenticates existing manifests/content/directories, and source references keep original block-generation fences. Inline canonical values and global hot-token exclusion are validated. New references in this restricted entry must already be visible to the original source pin; same-batch proposals remain pending integration.

At most two original FieldPageStore COW images retain same-slot cold bindings. Typed overwrite clears unchanged-slot proof. Exact cold fields/masks/bindings and group totals update with original hot locators/liveness preserved. Removing the last cold row erases its private slot; live/propertyless groups remain. Failures discard private images without consuming physical/logical IDs or changing file/root/I/O epoch. Owner insertion/deletion authority is not granted.

13 targeted cases pass, including existing hot planning/binding guards plus Node replacement/neighbor bytes and trusted pool binding, Edge existing-owner NULL insertion, hot/cold-only clearing, competing ROW rejection, hot conflicts/duplicates, actual LargeValues and unrelated damaged cold blocks. Initial10 cases exposed3 failures caused by passing target generation to a published-snapshot inspector. Corrected source uses the original pin fence; the same10 cases pass, then13 expanded cases pass. Failed raw results and generating scripts are retained; no checks/test assertions were relaxed.

Both experimental membership configurations pass **489 related module tests**, default **153**, zero failures/skips. This is private complete-owner-root preparation only, not formal Native field mutation or public lifecycle acceptance. Block-growth splitting, membership relocation, GroupDirectory/binding changes, merging hot/cold changes, batch-local new values, schema/constraint/index coordination, owner liveness/demotion/delete/inverse, atomic publication and complete old-pin/reclamation/recovery/integrity lifecycle remain pending before production guards open. Complete space admission and final P7 matrix remain required.
''')
print(checks)
