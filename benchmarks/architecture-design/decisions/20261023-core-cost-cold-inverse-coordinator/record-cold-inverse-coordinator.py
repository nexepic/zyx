from pathlib import Path
from datetime import datetime,timezone
import subprocess,json,hashlib,shutil,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-inverse-coordinator';d.mkdir(exist_ok=True)
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
checks={};libs={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['cold-inverse-final-legacy-','cold-inverse-final-compact-'],521,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],154,'zyx-build-core-candidate')]:
 p=Path('/workspace')/build/'libzyx_core.a';libs[profile]={'path':str(p),'sha256':sha(p),'binary_preserved':False};src=raw/folder
 for stem in stems:
  j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  receipt=json.loads((src/(stem+'receipt.json')).read_text());assert receipt['library_sha256']==libs[profile]['sha256'] and receipt['returncode']==0
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
src=raw/'cold-block-preparation-hybrid';stem='cold-inverse-lifecycle-';j=json.loads((src/(stem+'tests.json')).read_text());checks['targeted']={k:j.get(k,0) for k in ['tests','failures','skipped']};assert checks['targeted']=={'tests':43,'failures':0,'skipped':0}
assert json.loads((src/(stem+'receipt.json')).read_text())['library_sha256']==libs['hybrid']['sha256']
for name in ['tests.json','tests.log','receipt.json','compile.json','link.json']:
 source=src/(stem+name) if name in ['tests.json','tests.log','receipt.json'] else src/name
 shutil.copy2(source,d/('targeted-'+name))
for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/('cold-inverse-integration-'+name),d/('pre-fix-'+name))
j=json.loads((d/'pre-fix-tests.json').read_text());assert j['tests']==40 and j['failures']==2
for name in ['integrate-cold-inverse-preparation.py','test-cold-inverse-integration.py','fix-cold-writer-snapshot-attachment.py','update-cold-inverse-docs.py','record-cold-inverse-coordinator.py','build-cold-block-preparation-hybrid-modules.py','run-cold-block-preparation-hybrid-modules.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'pre_fix':{'tests':40,'failures':2,'cause':'nested openSnapshot under already-held metadata writer while reading actual large cold values'},'implemented':['Private original Native inverse coordinator entry with unconditional before-IO refusal','Complete authenticated cold/hot ROW projection, real owner namespaces, source field retention, original ID and manifest planning','Reuse complete existing cold whole-root carriers; allocate combined ROW carrier when hot fields must be merged','Private already-owned source-pin attachment to original LargeValueStore/SlottedPagePool; no nested snapshot or reconciliation','Propertyless live owners, quota and late cancellation refund and retry, existing large-source field mutation'],'remaining':['Actual cold public conversion/atomic installation','Owner creation/deletion/liveness/final-group cleanup and remaining schema combinations','Whole-space staged target admission and full public lifecycle','Remaining primary query costs and final P7']},indent=2)+'\n')
(d/'README.md').write_text('''# Original Native cold inverse preparation and existing-source pin attachment

Progress90%, P7 open.43 targeted checks,521 related module checks in each experimental membership variant, and154 default module checks pass with matching stable library hashes. No full suite, timing matrix, commit or push. Exact cumulative source is source.patch plus additional-source.tar.gz against round.json base HEAD; scripts are development evidence, not a standalone final replay recipe. No duplicate intermediate binaries.

A private exact-pin candidate context exercises the original inverse coordinator through complete ROW projection, field-page ownership/COW retention, immutable/manifests/content-index and physical placement planning. It requires candidate publication disabled and unconditionally refuses before physical writes. Public cold-mode guards stay closed. Authenticate cold block/owner/field totals, original block generation/manifests, global hot exclusion, competing ordinary ROW and legacy selector. A complete existing cold whole-root carrier can be reused only when no hot field must be merged; otherwise prepare a new complete ROW carrier. Node/Edge namespaces, live propertyless owners and original IDs remain intact.

Actual large-source cases exposed two nested-snapshot lock exceptions in the original40-case report, preserved as pre-fix evidence. ReadContext now attaches its already-owned source pin to the original LargeValueStore and SlottedPagePool through private constructors, preserving original allocator source validation without acquiring another snapshot or reconciling. No public constructor or independent publication authority changes. Fixed cases cover large-root mutation under writer, hot/cold inverse, exact carrier reuse, actual Edge64 with no Node64, live propertyless groups, quota failure, late cancellation after private ID reservation, full refunds and retry. Source file size/roots/epoch/public counters remain unchanged.

This is zero-write preparation against a private candidate descriptor, not public conversion or installation acceptance. Actual atomic cold installation, creation/deletion/liveness/final group, other schema combinations, whole-space staged target admission, full public lifecycle, remaining primary query costs and final P7 remain pending. No bounds or performance gates changed.
''')
print(checks)
