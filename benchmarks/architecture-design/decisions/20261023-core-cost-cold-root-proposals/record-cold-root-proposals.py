from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-root-proposals';d.mkdir(exist_ok=True)
checks={};libraries={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-root-proposals-legacy-','cold-root-proposals-compact-'],500,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],153,'zyx-build-core-candidate')]:
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libraries[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('cold-root-proposals-',24,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
for name in ['add-cold-root-proposals.py','test-cold-root-proposals.py','update-cold-root-proposals-docs.py','record-cold-root-proposals.py','merge-cold-block-hot.py','add-cold-hot-merge-tests.py','update-cold-hot-merge-docs.py','record-cold-hot-merge.py','add-cold-block-preparation.py','add-cold-block-preparation-tests.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','update-cold-block-preparation-docs.py','record-cold-block-preparation.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libraries,'implemented':['Original immutable-carrier proposal proof for explicitly changed unpublished cold roots','Reserved batch ID, full reference/database/source-generation/owner-envelope and actual payload length/directory/count/canonical-value checks','Global hot-token exclusion, preserve owner-neutral cross-owner and field/root carrier dedup','Existing source rows retain original-block-generation/pinned-manifest validation; no unpublished manifest read','Actual8193byte proposal, missing proof/unreserved ID/hot-token refusal, no source writes and account refunds'],'remaining':['Actual Native dispatch feeding same proposal holders to original immutable/manifest/content-index installation','Global hot demotion, owner liveness/delete and actual inverse installation/retirement','Public atomic conversion/mutation/full lifecycle','Whole-space/staged-target bootstrap admission and remaining primary read costs','Final P7 performance matrix']},indent=2)+'\n')
(d/'README.md').write_text("""# Batch-local unpublished cold-root carrier admission

Accepted progress **90%**, P7 open. No full suite/timing/commit/push. Original format/quota/bounds unchanged. Source/reports/recipes/current library fingerprints retained without duplicating intermediate binaries.

Cold preparation can borrow original immutable HybridPropertyLargeFieldPreparation candidates. An explicit target reference must use a reserved batch ID and match the full reference/database/source generation and owner-keyed envelope. Actual owned carrier payload length/directory/count/canonical values and global hot-token exclusion are checked before accepting it without reading an unpublished manifest. Owner-neutral payloads preserve cross-owner dedup and identical field/whole-root carrier reuse; proposal.token cannot invalidate an otherwise identical legal carrier. Existing SOURCE rows never use this proof: their original block-generation/pinned-manifest checks remain, preventing laundering old corruption.

24 targeted cases pass. New cases use actual8193-byte root proposals, reject missing proof/unreserved IDs/hot-token conflicts, and preserve legal single-field carrier reuse as a whole root. Alternative failed batches are discarded, not retried. Files/roots/I/O epoch/source counters remain unchanged; accounts refund. Both experimental memberships pass500 related module cases, default153, no failures/skips.

Actual Native dispatch must pass the same retained proposal holders to original immutable/manifest/content-index installation and atomic publication; this stage does not claim that integration. Global demotion, owner lifecycle/delete/inverse installation/retirement, public atomic conversion/full lifecycle, complete-space/staged-target bootstrap admission, remaining primary read costs and final P7 remain pending. No performance/adoption claim follows from these private preparation tests.
""")
print(checks)
