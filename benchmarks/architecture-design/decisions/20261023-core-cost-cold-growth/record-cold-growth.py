from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-growth';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-growth-legacy-','cold-growth-compact-'],491,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],153,'zyx-build-core-candidate')]:
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('growth-',15,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
for name in ['add-cold-split-codec.py','add-cold-growth-placement.py','add-cold-growth-tests.py','update-cold-growth-docs.py','record-cold-growth.py','merge-cold-block-hot.py','add-cold-hot-merge-tests.py','update-cold-hot-merge-docs.py','record-cold-hot-merge.py','add-cold-block-preparation.py','add-cold-block-preparation-tests.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','update-cold-block-preparation-docs.py','record-cold-block-preparation.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
for profile,stem in [('hybrid',''),('default','')]:
 src=raw/('cold-row-codec-'+profile);j=json.loads((src/(stem+'tests.json')).read_text())
 check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':19,'failures':0,'skipped':0};checks['codec-'+profile]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('codec-'+profile+'-'+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/('codec-'+profile+'-'+name))
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'implemented':['Shared source/owner merging for single and complete multi-block repack results','Original4KiB/physical record and original-generation fences, no prefix escapes on failure','Same-slot overwrite when fitting; otherwise original batch private IDs/PageImages and exact cold owner bindings','Actual group/membership co-location after growth relocation with GroupDirectory upsert','Existing Native directory overlay now consumes group upserts; publication guards remain closed','Valid63owner growth/relocation fixture with unchanged unrelated cold root and source files/roots/epoch, original account refunds'],'remaining':['Same-batch multiple affected blocks/owners and new values','Global hot demotion, owner liveness/delete/inverse','Actual Native field dispatch/schema/index/constraint and public atomic publication/lifecycle','Whole-space admission and final P7 performance matrix']},indent=2)+'\n')
(d/'README.md').write_text("""# Bounded cold growth, relocation and original directory edits

Accepted progress **90%**, P7 open. No full suite/timing/commit/push. No format/quota/bound changes. Source/reports/recipes/current library fingerprints retained, preceding frozen pairs protected.

Single/multi-block repacking shares original source validation and owner merging, with original-generation LargeValue fences. Complete owned results retain original4KiB/physical record bounds and original conversion RAW-safe splitting; later oversized input releases the prepared prefix. The existing preparer keeps a fitting single slot, otherwise erases the old private slot, uses original batch private logical IDs/PageImages, and records exact per-owner cold locations/totals. Membership/group relocate together if needed, preserving hot locators/liveness and emitting GroupDirectory upserts. The existing Native directory overlay consumes these upserts; formal mode3 dispatch/publication remains closed.

A valid source with an untouched legitimate large cold row creates page pressure.63 growing owners generate63 bounded blocks, force membership/group relocation and a new directory locator. Assertions check all canonical rows/bindings/totals, group co-location, untouched cold location, no independent physical/ID publication or file/root/epoch change, and account refunds. Codec coverage proves no prefix escapes when a later row cannot fit.15 targeted preparation cases and19 codec/binding cases pass. Both experimental memberships pass491 related module cases; default153 and default codec19 pass, no skips/failures. The copied codec runner's stale11/37 receipt label was corrected to actual13/39 with an explicit correction note; library hash/results were not changed.

Multiple affected blocks/owner merging, batch-local new values, global demotion, owner lifecycle/delete/inverse, actual Native field dispatch/schema/constraint/index/public atomic lifecycle, whole-space admission and final P7 remain pending. These private mechanism checks are not public conversion or performance acceptance.
""")
print(checks)
