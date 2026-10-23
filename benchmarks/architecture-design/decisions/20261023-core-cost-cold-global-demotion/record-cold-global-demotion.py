from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-global-demotion';d.mkdir(exist_ok=True)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
checks={};libs={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-global-demotion-final-legacy-','cold-global-demotion-final-compact-'],514,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],154,'zyx-build-core-candidate')]:
 p=Path('/workspace')/build/'libzyx_core.a';libs[profile]={'path':str(p),'sha256':sha(p),'binary_preserved':False};src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  receipt=json.loads((src/(stem+'receipt.json')).read_text());assert receipt['library_sha256']==libs[profile]['sha256'] and receipt['returncode']==0
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
src=raw/'cold-block-preparation-hybrid';stem='cold-global-demotion-edge-';j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':36,'failures':0,'skipped':0};checks['targeted']=check
assert json.loads((src/(stem+'receipt.json')).read_text())['library_sha256']==libs['hybrid']['sha256']
for name in ['tests.json','tests.log','receipt.json','compile.json','link.json']:
 source=src/(stem+name) if name in ['tests.json','tests.log','receipt.json'] else src/name
 shutil.copy2(source,d/('targeted-'+name))
control=raw/'cold-global-source-control'
for name in ['compile.json','compile.log','link.json','link.log','tests.json','tests.log','receipt.json']:shutil.copy2(control/name,d/('control-'+name))
shutil.copy2(control/'include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp',d/'control-without-source-auth.hpp')
c=json.loads((control/'tests.json').read_text());assert c['tests']==1 and c['failures']==1
assert json.loads((control/'receipt.json').read_text())['library_sha256']==libs['hybrid']['sha256']
for name in ['allow-private-cold-demotion-plans.py','prepare-cold-global-demotion.py','native-global-cold-demotion.py','test-cold-global-demotion.py','verify-cold-demotion-values.py','authenticate-demotion-cold-source.py','compare-cold-global-source-auth.py','update-cold-global-demotion-docs.py','record-cold-global-demotion.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'limits':{'actual_owners':64,'native_user_replacements':64,'total_column_plans':64,'actual_emitted_memberships':4096,'conversion_groups':8,'bytes':1048576,'default_non_demotion_combined_change_limit':64},'deliberate_control':{'tests':1,'failures':1,'difference':'omit only original cold-directory authentication during global hot authority removal','current_source_passes_same_case':True},'implemented':['Explicit preparation-only column demotion/merge; defaults and all liveness/public guards preserved','Original Native producer constructs final roots for all original/final column owners and exact cold-field changes','Remove column/membership, compare every demoted canonical value and merge affected cold blocks in same private batch','Original cold directories/Entity/ROW/selector/manifests authenticated under original hot set and block generation even for unrepacked blocks','Node/Edge, NULL, absent fields, actual8193byte carrier, merged17-removal threshold, missing-hot incompatible insertion, live propertyless group and source/value refusal coverage'],'remaining':['Atomic physical installation and other existing schema combinations','Pending/new/deleted-owner/liveness/final-group/inverse lifecycle, including actual public conversion','Whole-space/staged-target bootstrap and full public lifecycle','Remaining primary query costs and final P7 matrix']},indent=2)+'\n')
(d/'README.md').write_text('''# Whole-column cold demotion in original Native preparation

Progress90%, P7 open.36 targeted cases and514 related module cases in each experimental membership variant pass, default154, no failures/skips. Current receipts match stable library hashes. No full suite/timing/commit/push. Exact cumulative source is source.patch plus additional-source.tar.gz against round.json base HEAD. Staged scripts are development evidence, not a complete final replay recipe. Intermediate binaries are not duplicated.

Explicit ColumnDelta preparation exposes full immutable final cells and merges before deciding density/type. Default calls, owner liveness and public publication remain guarded. Original Native coordination merges every actual source/final column member into complete cold roots, preserving other fields, NULL/missing, original IDs and Node/Edge namespaces. Column/membership removal and cold-root repacking remain one private MetadataWriteBatch using existing directories, immutable/manifest/content-index and physical placement. No OwnerMap/EXT/cache/backend is added.

Every surviving membership requires a target root. Every demoted field's canonical bytes must match its complete immutable plan, including original same-batch carrier proof. Omitted owners/fields and wrong values refuse. Removing global hot authority also changes legality of every original cold directory in the group, so authenticate them under ORIGINAL hot tokens and block generation, even blocks not repacked. Preserve actual Entity/competing ROW/legacy-selector/manifest checks. A single-object/header-override control removing only this authentication lets a formerly forbidden token in a separate unrepacked cold owner reach completed preparation; fixed source refuses. Its deliberate failure is separately retained, not a current failure or performance result; generated executable removed after receipt creation.

Limits stay64 actual owners/64 Native user replacements/64 total column plans/4096 actual emitted memberships/1MiB, with original8group conversion bound untouched. Structural plans and roots describe overlapping owners, not extra owners. Non-demotion calls retain their original combined64change limit. Tests cover all64 actual values/NULL/old fields, complete and malformed target roots, merged17-removal threshold, incompatible missing-member insertion, actual8193-byte carriers, clearing all hot fields while keeping live propertyless owners, and actual Edge64 where Node64 does not exist. Files/roots/epoch/public counters remain unchanged; accounts refund.

This is original coordinator preparation before first physical IO against a private candidate descriptor, not public conversion/installation acceptance. Atomic installation, other schema combinations, pending/new/deleted-owner/liveness/final-group/inverse, whole-space/staged-target admission, full public lifecycle, remaining primary query costs and final P7 remain pending.
''')
print(checks)
