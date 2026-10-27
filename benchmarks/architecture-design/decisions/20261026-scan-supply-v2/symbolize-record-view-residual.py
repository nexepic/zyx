from pathlib import Path
from collections import defaultdict,Counter
import json,subprocess
D=Path(__file__).resolve().parent;summary={}
for case in ['record-view-residual/samples']:
 lines=(D/(case+'.pcs')).read_text().splitlines();groups=defaultdict(list)
 for line in lines[1:]:
  module,address,weight,phase=line.split('\t');groups[module].append((address,int(weight)))
 leaves=Counter();weighted=Counter()
 for module,entries in groups.items():
  symbol_file=Path('/tmp/zyx-libc-symbols/usr/lib/debug/.build-id/5e/5b51f4c39af9ac589408c1dc432ba390a273cc.debug') if module=='/lib/x86_64-linux-gnu/libc.so.6' else Path(module)
  if symbol_file.is_file():
   p=subprocess.run(['/workspace/zyx-sysroot/usr/bin/llvm-symbolizer-19','--obj='+str(symbol_file),'--output-style=JSON','--no-inlines'],input='\n'.join('0x'+x[0] for x in entries)+'\n',text=True,capture_output=True,check=True)
   values=[]
   for line in p.stdout.splitlines():
    if line.strip():
     value=json.loads(line);values.extend(value if isinstance(value,list) else [value])
   assert len(values)==len(entries),(module,len(values),len(entries))
   names=[(x.get('Symbol') or [{}])[0].get('FunctionName','') for x in values]
  else:names=['']*len(entries)
  for name,(_,weight) in zip(names,entries):
   name=name if name and name!='??' else 'unresolved '+module
   leaves[name]+=1;weighted[name]+=weight
 summary[case]={'header':lines[0],'independent_samples':sum(leaves.values()),'leaves':leaves.most_common(),'overrun_weighted_leaves':weighted.most_common(),'quality':json.loads((D/'record-view-residual/receipt.json').read_text())['within_five_percent']}
(D/'record-view-residual-leaves.json').write_text(json.dumps(summary,indent=2)+'\n')
for case,record in summary.items():
 print(case,'quality',record['quality'],'samples',record['independent_samples'])
 for name,count in record['leaves'][:15]:print(count,name)
