from pathlib import Path
from datetime import datetime,timezone
import json,subprocess,shutil,hashlib,tarfile
r=Path('/workspace/zyx'); d=r/'benchmarks/architecture-design/decisions/20261023-core-cost-cold-compatible-hot'; raw=r/'benchmarks/architecture-design/results/20261022-core-cost'; checks={};libs={}
for profile,stems,total,build in [('hybrid',['compatible-binding-legacy-','compatible-binding-compact-'],481,'zyx-build-core-hybrid'),('default',[''],153,'zyx-build-core-candidate')]:
 src=raw/('cold-row-ownership-'+profile)
 for stem in stems:
  report=json.loads((src/(stem+'tests.json')).read_text()); check={k:report.get(k,0) for k in ['tests','failures','skipped']}; assert check=={'tests':total,'failures':0,'skipped':0}; checks[profile+'-'+(stem or 'final')]=check
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 p=Path('/workspace')/build/'libzyx_core.a'; libs[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'binary_preserved':False,'format':'13/39' if profile=='hybrid' else '5/31'}
for name in ['add-cold-compatible-hot.py','add-cold-source-binding-proof.py','update-cold-compatible-hot-docs.py','record-cold-compatible-hot.py','build-cold-compatible-hot-hybrid-modules.py','build-cold-compatible-hot-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:
 p=Path('/tmp')/name
 if p.exists():shutil.copy2(p,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
untracked=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in untracked:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'recorded_at_utc':datetime.now(timezone.utc).isoformat(),'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'preceding_frozen_pair':'20261023-core-cost-cold-native-probe','implemented':['Private compatible hot value/NULL/presence plans preserve cold references, totals and bytes','Merged plans recheck threshold; 16 removals retaining48 prepare, 17 dropping47 reject','Only authenticated FieldPageStore clones mint unchanged-slot proof; new/open images have none','Insert/overwrite/erase clear slot proof; insertion failure clears all proof','Trusted binding leaves unvisited unchanged cold bytes intact, while new/modified records retain full validation','Fresh and reused slots cannot inherit proof; first cold access still detects corrupt unchanged source'],'not_accepted':['Actual mode3 mutation/publication','Cold update/repack, global demotion, owner liveness/delete/inverse','Public conversion integrity, reclamation, old pins, recovery and whole space admission','Final P7 performance matrix'],'performance_claim':None},indent=2)+'\n')
(d/'README.md').write_text('''# Compatible hot private preparation and unchanged cold binding

Accepted progress remains **90%**, P7 open. No benchmark, full suite, commit or push.

Private compatible hot value/NULL/presence plans retain cold references, exact totals and bytes. Merging rechecks the column threshold: 16 removals retaining48 prepare; 17 dropping47 reject. Public mutation guards remain closed. Cold tokens, incompatible types, owner deletion and demotion gain no write authority.

Only authenticated FieldPageStore clones mint transient original-slot proof. New/open images cannot mint it. Private clones inherit proof; central insert/overwrite/erase clear affected proof, and insertion failure clears all proof. Binding preserves unchanged unvisited cold records without decompression; new/modified/other records remain fully validated. Tests cover corrupt cold bytes, untrusted reopen, fresh/reused slots, missing-hot NULL insertion on Edge, exact totals, unchanged files/roots and refunded accounts. Corruption remains detectable on first cold access or complete integrity. The fixed bitmap uses the existing holder account and is not serialized or a new cache.

Both experimental membership configurations pass **481 related module tests**; default passes **153**. Source, raw reports, recipes and current library fingerprints are preserved. Intermediate binaries are not duplicated; the preceding Native-probe frozen pair remains available. These private planning/binding tests do not establish public conversion lifecycle or performance acceptance. Cold repacking, global demotion, owner liveness/delete/inverse, atomic publication and complete actual-conversion lifecycle remain pending.
''')
print(checks)
