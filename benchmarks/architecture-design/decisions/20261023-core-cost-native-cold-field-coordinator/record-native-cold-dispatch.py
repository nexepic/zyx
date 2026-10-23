from pathlib import Path
from datetime import datetime,timezone
import json,hashlib,subprocess,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-native-cold-field-coordinator';d.mkdir(exist_ok=True)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
checks={};libs={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['native-cold-dispatch-stable-legacy-','native-cold-dispatch-stable-compact-'],507,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],154,'zyx-build-core-candidate')]:
 p=Path('/workspace')/build/'libzyx_core.a';libs[profile]={'path':str(p),'sha256':sha(p),'binary_preserved':False}
 src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  receipt=json.loads((src/(stem+'receipt.json')).read_text());assert receipt['library_sha256']==libs[profile]['sha256'] and receipt['returncode']==0
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('native-cold-coordinator-',26,2),('native-cold-coordinator-store-order-',26,2),('native-cold-dispatch-stable-target-',29,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
 if not failures:assert json.loads((src/(stem+'receipt.json')).read_text())['library_sha256']==libs['hybrid']['sha256']
for name in ['add-native-cold-preparation.py','add-native-cold-coordinator-tests.py','native-cold-field-dispatch.py','add-native-cold-dispatch-tests.py','update-native-cold-dispatch-docs.py','record-native-cold-dispatch.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
for name in ['native-cold-dispatch-hybrid-stable-build.log','native-cold-dispatch-default-stable-build.log']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'source_files':{str(p.relative_to(r)):sha(p) for p in [r/'src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp',r/'include/graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp',r/'include/graph/storage/relocatable/metadata/HybridPropertyPoolPlacement.hpp',r/'tests/include/storage/relocatable/property/HybridPropertyNativeWriteTestAccess.hpp',r/'tests/src/storage/relocatable/property/test_HybridPropertyColdCompatibleHot.cpp']},'evidence_limit':'Actual original Native coordinator preparation consumes an in-memory candidate descriptor only; all candidate sources unconditionally refuse before physical IO. No public conversion/new-mode installation/Catalog-index-constraint lifecycle or performance acceptance is claimed.','remaining':['Real Catalog/index/constraint source coverage and atomic physical installation','Global hot demotion; pending/new/deleted-owner/liveness/final-group/inverse lifecycle','Whole-space/staged-target bootstrap admission and full public-conversion lifecycle','Remaining primary query costs and final P7 matrix']},indent=2)+'\n')
(d/'README.md').write_text('''# Native cold-field coordinator preparation

Progress90%, P7 open. No full suite/timing/commit/push.29 targeted cases and507 related module cases in each experimental membership variant pass, default154, no failures/skips. Final test receipts match current stable library hashes. Exact cumulative source is source.patch plus additional-source.tar.gz against round.json’s base HEAD. Staged editing scripts are retained as development evidence, not an independently complete final replay recipe. Intermediate binaries are not duplicated.

Original Native field coordination now dispatches by globally hot tokens, merges same-column hot changes, borrows complete owner cold fields, applies final cold replacements/deletions and prepares only affected cold blocks. NULL/missing/no-op semantics, original canonical/compression limits, exact source fences, Entity/competing ROW checks and one-MiB accounting remain. Whole-root proposals share original largeFields holders and byte-level cross-owner dedup with existing immutable/manifest/content-index planning. No independent LargeValue write or unpublished-manifest read. Release the copied membership’s physical pin before cold reads and admit reference encoding before temporary construction.

Fix the existing Native directory loop’s outer filter to include groups-only upserts; previously it could skip the GroupDirectory edit already consumed in the inner loop. A valid63-owner growth case exercises splitting/relocation through this original loop and observes nonempty prepared directory images. New cases also cover same-column hot changes plus cold edits, NULL creation, deleting the last selected cold root, missing-delete no-op, two-owner identical8193-byte root dedup, another-pin/source-ROW conflict and refunds with unchanged files/roots/epoch/public counters.

The private laboratory entry accepts only the exact source pin/file/metadata and no compound graph publication. It always refuses before first physical IO after original field/directory/derived/value/placement planning. Normal callers retain durable-descriptor and cold-mode guards. There is no optional publication bypass. This is not actual public conversion or new-mode physical installation. Test access reserves the same16KiB source-store frontier as the public entry. Original failures are preserved: read-only stores must be constructed before the writer; the fixture’s direct compact-flag Bootstrap advancement needs real writer-admission external rebase before measuring zero-write preparation. No production validation/test requirement was weakened.

Real Catalog/index/constraint sources, atomic physical installation, global demotion/new-owner/deletion/liveness/final-group/inverse, whole-space/staged-target admission, full public lifecycle, remaining primary query costs and final P7 stay pending.
''')
print(checks)
