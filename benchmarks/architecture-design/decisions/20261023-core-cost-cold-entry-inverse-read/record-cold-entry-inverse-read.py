from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-entry-inverse-read';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-entry-inverse-legacy-','cold-entry-inverse-compact-'],498,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],153,'zyx-build-core-candidate')]:
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('fresh-cold-',18,1),('fresh-cold-corrected-',18,0),('cold-inverse-projection-',22,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
for name in ['add-fresh-cold-block.py','test-fresh-cold-block.py','add-cold-inverse-projection.py','update-cold-entry-inverse-read-docs.py','record-cold-entry-inverse-read.py','run-cold-row-codec-hybrid-modules.py','run-cold-row-codec-default-modules.py','merge-cold-block-hot.py','add-cold-hot-merge-tests.py','update-cold-hot-merge-docs.py','record-cold-hot-merge.py','add-cold-block-preparation.py','add-cold-block-preparation-tests.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','update-cold-block-preparation-docs.py','record-cold-block-preparation.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
for profile,stem in [('hybrid','cold-entry-inverse-final-'),('default','')]:
 src=raw/('cold-row-codec-'+profile);j=json.loads((src/(stem+'tests.json')).read_text())
 check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':20,'failures':0,'skipped':0};checks['codec-'+profile]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('codec-'+profile+'-'+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/('codec-'+profile+'-'+name))
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'implemented':['Canonical empty source for fresh private byte preparation with no fabricated source page or nonexistent-row removal','Fresh roots require actual live owners without another cold authority, retaining other hot/cold owners and exact totals','First cold field on a propertyless live group through existing batch private IDs/PageImages','Existing inverse RowPreparation read-only lookup supports cold roots with original Entity/owner/value/budget authentication','Original public groupRecord guard remains; inverse publisher explicitly retains it before projection','Actual8193byte LargeValue, NULL/missing/propertyless/Node/Edge and corrupt CRC refund coverage'],'initial_fixture_correction':'hasFields returns optional<bool>; engaged false must compare equal optional(false), not EXPECT_FALSE(optional). Reading implementation unchanged; failed raw evidence retained.','remaining':['Actual Native field dispatch and same-batch new values','Global demotion, owner liveness/delete and actual inverse install/retirement','Public atomic conversion/mutation/reopen/fault/integrity/old-pin lifecycle','Whole-space/staged-target bootstrap admission and remaining primary read costs','Final P7 performance matrix']},indent=2)+'\n')
(d/'README.md').write_text("""# Fresh cold entries and inverse read projection

Accepted progress **90%**, P7 open. No full suite/timing/commit/push. Original format/quota/bounds unchanged. Source/reports/recipes/current library fingerprints retained without duplicating binaries.

The original codec accepts an empty source for fresh private preparation only: validate group/kind/generations/canonical owner/count/reference constraints, reject removal of nonexistent rows, and return accounted whole results. Only canonical empty references request creation in the original preparer. New roots require actual live Entity Map owners with no competing cold root, granting no entity creation authority. No fabricated source page/retirement is performed. Other hot/cold authorities/totals remain, including a first cold field on an entirely propertyless live group. Original batch private IDs/PageImages remain unpublished until the enclosing coordinator succeeds.

Existing inverse RowPreparation now performs read-only mode3 lookup, retaining actual Entity authentication, owner enumeration and original ValueReader/accounted decoding. Original public groupRecord guards remain, and the inverse publisher explicitly retains that guard before projection. Read support does not enable cold inverse installation/publication. Tests cover actual8193-byte LargeValue, NULL/missing/propertyless owners, Node/Edge identity/exact counts and corrupt-CRC refunds.

22 targeted cases pass. The initial18-case selection had one assertion mistake: hasFields returns optional<bool>, so routed false is engaged. The assertion now checks optional(false); reading source was not changed. Original failed report/generating test script and corrected18-case report remain. Both experimental memberships pass498 related module cases, default153. Both final-library codec/binding selections pass20 cases. No failures/skips in final selections.

Actual Native field dispatch/batch-local new values, global demotion, owner liveness/delete, actual inverse installation/retirement, public atomic conversion/mutation/reopen/fault/integrity/old-pin lifecycle, complete-space/staged-target bootstrap admission, remaining primary read costs and final P7 matrix remain pending. These are private mechanism/read projections, not completed public conversion or performance acceptance.
""")
print(checks)
