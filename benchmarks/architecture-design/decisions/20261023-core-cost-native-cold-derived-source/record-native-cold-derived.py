from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-native-cold-derived-source';d.mkdir(exist_ok=True)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
checks={};libs={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['native-cold-derived-legacy-','native-cold-derived-compact-'],508,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],154,'zyx-build-core-candidate')]:
 p=Path('/workspace')/build/'libzyx_core.a';libs[profile]={'path':str(p),'sha256':sha(p),'binary_preserved':False};src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  receipt=json.loads((src/(stem+'receipt.json')).read_text());assert receipt['library_sha256']==libs[profile]['sha256'] and receipt['returncode']==0
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
src=raw/'cold-block-preparation-hybrid'
for stem,total,failures in [('native-cold-schema-',30,1),('native-cold-schema-stage-',1,1),('native-cold-schema-stage2-',1,1),('native-cold-schema-create-',1,0),('native-cold-derived-target-',30,0)]:
 j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':failures,'skipped':0};checks['targeted-'+stem]=check
 for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/('targeted-'+stem+name))
 if stem=='native-cold-derived-target-':assert json.loads((src/(stem+'receipt.json')).read_text())['library_sha256']==libs['hybrid']['sha256']
control=raw/'native-cold-derived-control'
for name in ['RelocatableMetadataStoreHybridMutation-before.cpp','compile.json','compile.log','link.json','link.log','tests.json','tests.log','receipt.json']:shutil.copy2(control/name,d/('control-'+name))
c=json.loads((control/'tests.json').read_text());assert c['tests']==1 and c['failures']==1
assert 'required_cold' in (control/'tests.log').read_text()
for name in ['add-native-cold-schema-test.py','compare-native-cold-derived-source.py','update-native-cold-derived-docs.py','record-native-cold-derived.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'deliberate_before_fix_control':{'tests':1,'failures':1,'difference':'only restore former roots.readEntityPropertyOptional old-value route in original Native coordinator, compiled with same core flags as an overriding object','current_source_passes_same_case':True},'remaining':['Other schema combinations and atomic physical installation','Global demotion/owner/delete/liveness/final-group/inverse lifecycle','Whole-space/staged-target bootstrap and full public-conversion lifecycle','Remaining primary query costs and final P7 matrix']},indent=2)+'\n')
(d/'README.md').write_text('''# Native derived reads share the authenticated field source

Progress90%, P7 open.30 targeted cases,508 related module cases in each experimental membership variant and154 default cases pass, no failures/skips. Current receipts match stable library hashes. No full suite, timing, commit or push. Exact cumulative source is source.patch plus additional-source.tar.gz against round.json base HEAD; editing scripts are development evidence rather than a complete final replay recipe.

Derived old-value lookups now use the same authenticated ReadContext as field preparation and fall back to the original ROW path only for actual ROW owners. This preserves the existing source while supporting candidate cold fields and avoids reopening an unrelated descriptor boundary for Native fields. Existing normal Native/ROW compound-publication modules remain passing.

The new coherent private candidate has real Catalog tokens/definitions, two real scalar index trees, an actual Node label and a NOT NULL cold-field definition. Updating a hot field must read unchanged cold data to satisfy the constraint and prepare scalar-index COW. Deleting the required cold field refuses. Original roots/index keys/files/epoch stay unchanged; accounts refund and public guards remain closed. This is candidate preparation, not public conversion or physical installation.

An identical-flags single-object control restores only the former old-value route; the SAME corrected source fixture then fails with required_cold NOT NULL violation because unchanged cold data is misread as missing. Current source passes. Control source/compile/link/test/fingerprints are retained separately; its deliberately failed test is not a current failure or a performance baseline. Its generated executable was removed after receipt creation. Initial source-setup failures are also retained: newly absent index trees require original stageScalarIndexCreate, not upsert. No production validation was relaxed.

Other schema combinations, atomic physical installation, global hot demotion/owner/deletion/liveness/final-group/inverse, whole-space/staged-target admission, full public lifecycle, remaining primary query costs and final P7 remain open.
''')
print(checks)
