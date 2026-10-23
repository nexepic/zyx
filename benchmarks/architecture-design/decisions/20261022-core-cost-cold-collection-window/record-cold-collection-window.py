from pathlib import Path
from datetime import datetime,timezone
import json,subprocess,shutil,hashlib,tarfile
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261022-core-cost-cold-collection-window';d.mkdir(exist_ok=True);raw=r/'benchmarks/architecture-design/results/20261022-core-cost';checks={};libs={}
for profile,stems,build,total in [('hybrid',['collection-legacy-','collection-compact-'],'zyx-build-core-hybrid',366),('default',[''],'zyx-build-core-candidate',119)]:
 src=raw/('cold-row-ownership-'+profile)
 for stem in stems:
  report=json.loads((src/(stem+'tests.json')).read_text());check={k:report.get(k,0) for k in ['tests','failures','skipped']};checks[profile+'-'+(stem or 'final')]=check
  assert check=={'tests':total,'failures':0,'skipped':0}
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 target=Path('/workspace')/('zyx-cold-collection-window-'+profile+'-candidate');target.mkdir(exist_ok=True);p=target/'libzyx_core.a';shutil.copy2(Path('/workspace')/build/'libzyx_core.a',p);libs[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'format':'13/39' if profile=='hybrid' else '5/31'}
for name in ['add-cold-collection-window.py','update-cold-collection-docs.py','record-cold-collection-window.py','build-cold-reachability-hybrid-modules.py','build-cold-reachability-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
untracked=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in untracked:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'implemented':['Complete collection directory/count, per-field pre-admission and owning-value construction share the same synchronous body','Multiple-field/exact-count selected reads share decoded body per ordered consecutive same-reference run in original64owner window','Synchronous stack lease into original point/count readers, no owning context cache or escaped decoded bytes','Borrowed output/count rectangles and translated sink ordinals; exception/nested parent restoration','Accounted fixed-size membership boundary protects against existing-cache reuse by nested consumers','Cold physical-page pin released after owned decoding and before hot/manifest reads','No quota replay after potentially observable output/count writes, original EntityMap/no competing ROW/selector preflight'],'collection_work_evidence':'Actual hot integer, cold long ID and8193-byte LargeValue text materialize with one block decode and exactly three field admissions. Owning result returns after lease exit, failure cannot replay construction/admission.', 'work_evidence':'Mixed hot/cold owning projection and exact counts decode shared block once, real LargeValue count-only likewise once, quota failure after output restores lease as proven by subsequent point read doing a new decode, bad cold CRC does not affect hot-only owning projection.','not_accepted':['Actual public mode3 conversion, complete mutation/reclamation/old-pin/retirement lifecycle','Mature whole allocation, staged migration bootstrap, final P7 performance matrix'],'performance_claim':None},indent=2)+'\n')
(d/'README.md').write_text("""# Complete-collection cold window

Accepted progress remains **90%**, P7 open. No timing benchmark, full unit suite, commit or push.

Complete collection count/directory, per-field admission and owning-value construction now borrow the same decoded cold body synchronously. Original owner/types/sorting/NULL/complex/LargeValue and slots-admission contracts remain. The owning result returns after lease exit; construction failures cannot trigger quota fallback/replayed admission. Pre-construction window admission may decline to the original point path. No owning context cache, new format or allowance is added.

Both experimental membership configurations pass **366 related module tests**, default **119**. The new real-LargeValue case materializes a hot integer, cold long ID and8193-byte text with one cold block decode and exactly three per-field admissions. Prior count/selected, exception/no-replay/lease-restoration, ownership, query/conversion/mutation, reclamation and journal modules remain green.

This is private-candidate component evidence, not real public mode3 conversion/query/lifecycle or latency acceptance. Public conversion and mutation/reclamation/old-pin/retirement lifecycle, mature whole allocation/staged migration bootstrap and final P7 matrix remain pending. Normal mode3 publication and mutation remain closed.
""")
print(checks)
