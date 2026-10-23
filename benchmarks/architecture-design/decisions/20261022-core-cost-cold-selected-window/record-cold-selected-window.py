from pathlib import Path
import json,subprocess,shutil,hashlib,tarfile
r=Path('/workspace/zyx');d=r/'benchmarks/architecture-design/decisions/20261022-core-cost-cold-selected-window';d.mkdir(exist_ok=True);raw=r/'benchmarks/architecture-design/results/20261022-core-cost';checks={};libs={}
for profile,stems,build,total in [('hybrid',['selected-initial-','selected-legacy-','selected-compact-'],'zyx-build-core-hybrid',361),('default',[''],'zyx-build-core-candidate',119)]:
 src=raw/('cold-row-ownership-'+profile)
 for stem in stems:
  report=json.loads((src/(stem+'tests.json')).read_text());check={k:report.get(k,0) for k in ['tests','failures','skipped']};checks[profile+'-'+(stem or 'final')]=check
  assert check['failures']==0 and check['skipped']==0
  if stem!='selected-initial-':assert check['tests']==total
  for name in ['tests.json','tests.log','receipt.json']:shutil.copy2(src/(stem+name),d/(profile+'-'+stem+name))
 for name in ['compile.json','link.json']:shutil.copy2(src/name,d/(profile+'-'+name))
 target=Path('/workspace')/('zyx-cold-selected-window-'+profile+'-candidate');target.mkdir(exist_ok=True);p=target/'libzyx_core.a';shutil.copy2(Path('/workspace')/build/'libzyx_core.a',p);libs[profile]={'path':str(p),'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'format':'13/39' if profile=='hybrid' else '5/31'}
for name in ['add-cold-selected-window.py','add-cold-selected-tests.py','update-cold-selected-docs.py','record-cold-selected-window.py','build-cold-reachability-hybrid-modules.py','build-cold-reachability-default-modules.py','run-cold-row-ownership-hybrid-modules.py','run-cold-row-ownership-default-modules.py']:shutil.copy2(Path('/tmp')/name,d/name)
(d/'source.patch').write_bytes(subprocess.check_output(['git','diff','--binary'],cwd=r))
untracked=subprocess.check_output(['git','ls-files','--others','--exclude-standard','include','src','tests','cmake'],cwd=r,text=True).splitlines()
with tarfile.open(d/'additional-source.tar.gz','w:gz') as tar:
 for name in untracked:tar.add(r/name,arcname=name)
(d/'round.json').write_text(json.dumps({'progress_percent':90,'P7_accepted':False,'full_suite':False,'timing_benchmark':False,'commit_push':False,'checks':checks,'libraries':libs,'implemented':['Single-field typed cold selection shares one decoded block per consecutive same-reference run within original64owner window','Actual batch EntityMap/no competing ROW and explicit selector checks before output','Ordered stable output ordinals, root-presence missing vs NULL, synchronous text/scalar borrows and original complex owning codec','Declines admission only before producing; output failures including quota propagate with no replay','No new context cache, decoded body escapes or budget allowance'],'work_evidence':'64 selected owners, two cold roots in one block, one cold block decode and64actual route checks, two texts and47root-present missing cells in original order. RAW Edge split blocks keep NULL separate from missing; actual LargeValue and quota failure after output covered.','not_accepted':['Multiple-field/exact-count window consolidation','Public mode3 conversion/mutation and full lifecycle including actual reclamation/old pins/retirement','Mature whole allocation, staged migration bootstrap, final P7 performance matrix'],'performance_claim':None},indent=2)+'\n')
(d/'README.md').write_text('''# Single-field cold selected window

Accepted progress remains **90%**, P7 open. No timing benchmark, full unit suite, commit or push.

Single-field typed cold selection shares one decoded body per consecutive same-reference run in the original64-owner window. Requests remain in ordinal order, including gaps with no cold row, and switching references releases the old body. Before output, actual Entity Map/no competing ROW and no legacy selectors authenticate requested owners. Globally hot tokens decline to hot routing, never cold fallback on hot absence. Borrowed primitive/text views remain synchronous, complex values retain the original owning codec, and missing cells differ from NULL. No decoded body is retained in the context and no cache/budget/format is added.

Both experimental membership configurations pass **361 related module tests**, default **119**. Four new cases demonstrate one decode for two shared cold roots in a64-owner request with64actual route checks, ordered47root-present missing cells, actual LargeValue and sink quota failure after output without replay, competing ROW rejection before output, and split RAW Edge blocks with NULL/missing. Initial30related checks also pass. Original query, conversion, mutation, ownership, reclaim and journal module selections remain green.

Only the single-field typed/no-count path is consolidated here. Multiple-field/exact-count windows, actual public mode3 conversion and complete mutation/reclamation/old-pin/retirement lifecycle, mature whole allocation/staged migration bootstrap and final P7 performance matrix remain pending. Private candidate fixtures are not production-adoption or latency evidence; normal mode3 publication/mutation remain closed.
''')
print(checks)
