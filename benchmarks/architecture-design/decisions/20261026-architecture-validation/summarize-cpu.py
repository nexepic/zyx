from pathlib import Path
from collections import Counter,defaultdict
import subprocess,json
r=Path('/workspace/zyx');o=r/'benchmarks/architecture-design/decisions/20261026-architecture-validation';p=Path('/tmp/zyx-architecture-diagnostic/current.pcs');g=defaultdict(Counter)
for l in p.read_text().splitlines()[1:]:
 lib,a,w,phase=l.split('\t');g[lib][a]+=int(w)
c=Counter()
for lib,items in g.items():
 keys=list(items)
 names=subprocess.run(['addr2line','-f','-C','-e',lib],input=''.join('0x'+a+'\n' for a in keys),capture_output=True,text=True).stdout.splitlines()[::2] if Path(lib).is_file() and 'libc.so' not in lib else ['UNRESOLVED '+lib]*len(keys)
 for a,n in zip(keys,names):c[n]+=items[a]
(o/'cpu-summary.json').write_text(json.dumps({'observer_header':p.read_text().splitlines()[0],'weighted_leaf_samples':c.most_common(40),'limitations':'No stage instrumentation; no stack attribution; includes warmups; unresolved leaves retained'},indent=2)+'\n')
(o/'all-nodes.pcs').write_bytes(p.read_bytes())
print(c.most_common(10))
