from pathlib import Path
from datetime import datetime,timezone
import json,hashlib,shutil,subprocess,tarfile
r=Path('/workspace/zyx');raw=r/'benchmarks/architecture-design/results/20261022-core-cost';d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-inverse-field-retention';d.mkdir(exist_ok=True)
checks={};libs={}
for profile,folder,stems,total,build in [('hybrid','cold-row-ownership-hybrid',['inverse-shared-final-legacy-','inverse-shared-final-compact-'],502,'zyx-build-core-hybrid'),('default','cold-row-ownership-default',[''],154,'zyx-build-core-candidate')]:
 for stem in stems:
  src=raw/folder;j=json.loads((src/(stem+'tests.json')).read_text());check={k:j.get(k,0) for k in ['tests','failures','skipped']};assert check=={'tests':total,'failures':0,'skipped':0};checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a';libs[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False}
for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(raw/'cold-row-ownership-hybrid'/('inverse-shared-target-'+name),d/('original-layout-assumption-'+name))
for name in ['implement-inverse-shared.py','record-inverse-shared.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
names=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in names:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'evidence_limit':'The actual eight-group converter currently creates distinct logical pages. Public multi-group inverse therefore verifies preservation of other groups, not the retained shared-frame publication branch. Separate private PageImage test verifies exact slot retention and source immutability. Cold inverse public guard remains closed.'},indent=2)+'\n')
(d/'README.md').write_text('''# Inverse field-page retention

Progress90%, P7 open. No full suite, benchmark, commit or push.

The inverse coordinator authenticates source-page ownership and removes only selected slots in private COW images. Duplicate/stale slot locators fail. Only empty logical pages enter the erased-location set; retained pages keep their logical PageId and other groups’ slot generations, with a fresh hybrid owner header in the original atomic publication. Old physical routes remain protected by the original snapshot/retirement protocol. Retained hybrid images use their own bounded packing cursor and preserve the native frame kind.

502 related module cases pass in both experimental membership variants,154 in default. Existing inverse fault/cancellation/quota/reopen/pin coverage remains passing. New public test converts eight groups, inverses only group1, verifies all other group authorities, old reads and integrity. A separate private shared PageImage test removes one group while preserving another’s slots/source bytes/reopen and rejects duplicate deletion.

Evidence limit: the initial new test incorrectly assumed the current converter shared logical pages across groups. Actual locator enumeration disproved this; its original failed report is retained. The corrected test reports actual shared-page count, currently0, and does NOT claim to exercise shared-frame publication. Private image coverage is not public converted-layout acceptance. Mode3 public inverse/write guards remain closed. Actual cold dispatch, global demotion/owner/delete/inverse installation, whole-space bootstrap, primary query costs and final P7 remain open.
''')
print(checks)
