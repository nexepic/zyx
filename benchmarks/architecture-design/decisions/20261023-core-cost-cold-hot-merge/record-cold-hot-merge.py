from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-hot-merge';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-hot-merge-legacy-','cold-hot-merge-compact-'],490,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],153,'zyx-build-core-candidate')]:
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('hot-merge-',14,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
for name in ['merge-cold-block-hot.py','add-cold-hot-merge-tests.py','update-cold-hot-merge-docs.py','record-cold-hot-merge.py','add-cold-block-preparation.py','add-cold-block-preparation-tests.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','update-cold-block-preparation-docs.py','record-cold-block-preparation.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'implemented':['Original hot-plan source database/generation/group/column authentication','Reuse original hot COW images for cold replacement without competing source copies','Preserve final hot value/presence/null bitmap, cold binding and exact hot/cold/group totals','Original combined64change bound; source-only FieldPageStore constructor arguments const','No Entity Map liveness/token/demotion authority or actual publication granted'],'remaining':['Block-growth splitting, membership relocation and GroupDirectory/binding atomic changes','Batch-local new values, actual Native field dispatch/schema/constraint/index coordination','Full owner/demotion/deletion/inverse and public publication/lifecycle','Whole-space admission and final P7 performance matrix']},indent=2)+'\n')
(d/'README.md').write_text("""# Shared hot/cold private page preparation

Accepted progress remains **90%**, P7 open. No timing/full suite/commit/push. Format and original quotas/bounds unchanged. Source/reports/recipes/current library fingerprints retained without copying intermediate binaries.

The existing preparer authenticates every hot plan through its original constructor, then cold replacement reuses those private COW images instead of cloning another competing old source page. Original cold binding/counts and hot token/locator ownership must remain unchanged. Source GroupDirectory/Entity Map/ROW/selector and actual manifests retain the preceding cold-block checks. Final matrix/group totals preserve hot value/presence/NULL changes and cold replacement in one image. Combined input is at most64 changes. Source-only constructor FieldPageStore references are const; no const_cast, new backend, cache or publication authority.

14 targeted cases pass, including both hot value replacement and missing-hot NULL insertion followed by cold row replacement on the same actual source page. The result has one page image, both changes retained, exact counts/bitmaps, unchanged files/roots/I/O epoch/IDs and refunded memory. Both experimental membership configurations pass **490 related module tests**, default **153**, no failures/skips.

Actual Native field dispatch, block splitting/membership relocation/GroupDirectory binding changes, batch-local new values, schema/constraint/index coordination, owner liveness/global demotion/deletion/inverse, full public-conversion publication/lifecycle, whole-space admission and final P7 matrix remain pending. These are private preparation checks, not accepted public mutation or performance evidence.
""")
print(checks)
