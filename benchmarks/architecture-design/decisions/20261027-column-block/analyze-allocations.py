"""Symbolize existing allocation samples; no new timing measurements."""
from pathlib import Path
from collections import defaultdict,Counter
import subprocess,json,hashlib
N=Path(__file__).resolve().parent;out=[]
for case in ('all_nodes_property_filter','label_scan_filter','multi_key_group'):
 p=N/'current-work'/(case+'.stacks'); groups=defaultdict(list); addresses=defaultdict(set)
 window=-1
 for line in p.read_text().splitlines():
  if line.startswith('# window '):
   window=int(line.split()[2]);continue
  if line.startswith('#') or window!=2:continue
  sample,size,frame,module,addr=line.split('\t')
  groups[int(sample)].append((int(frame),module,addr));addresses[module].add(addr)
 symbols={}
 for module,items in addresses.items():
  items=sorted(items)
  if not Path(module).is_file():continue
  lines=subprocess.check_output(['/workspace/zyx-sysroot/usr/bin/llvm-symbolizer-19','--obj='+module,'--output-style=JSON','--no-inlines'],input='\n'.join('0x'+x for x in items)+'\n',text=True).splitlines()
  values=[]
  for line in lines:
   x=json.loads(line);values.extend(x if isinstance(x,list) else [x])
  assert len(values)==len(items)
  for a,x in zip(items,values):symbols[module,a]=(x.get('Symbol') or [{}])[0].get('FunctionName','')
 counter=Counter();inclusive=Counter(); callers=Counter(); call_paths=Counter()
 for frames in groups.values():
  names=[symbols.get((module,addr),'') for _,module,addr in sorted(frames)]
  for i,n in enumerate(names):
   if 'BudgetMemoryResource::do_allocate' in n:
    callers[next((v for v in names[i+1:] if v and v != n), 'unresolved')]+=1
    call_paths[tuple(v for v in names[i+1:i+6] if v)]+=1;break
  engine=[n for n in names if n.startswith('graph::')]
  counter[engine[0] if engine else 'unresolved or non-engine stack']+=1
  inclusive.update(set(engine))
 out.append({'case':case,'source_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'window':2,'sample_interval':256,'sampled_allocations':len(groups),'first_engine_frame':counter.most_common(),'budget_immediate_callers':callers.most_common(),'budget_call_paths':call_paths.most_common(),'inclusive_stack_frames':inclusive.most_common(),'limitations':'Every 256th allocation sampled; counts are not byte shares or timings. Inclusive frames overlap; stacks can truncate. No performance acceptance claim.'})
(N/'current-work'/'allocation-attribution.json').write_text(json.dumps(out,indent=2)+'\n')
for r in out:
 print(r['case'],r['sampled_allocations'])
 for name,count in r['first_engine_frame'][:8]:print(count,name[:180])
