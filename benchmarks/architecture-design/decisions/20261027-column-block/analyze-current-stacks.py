"""Symbolize diagnostic stacks; preserve per-sample frames and observer quality."""
from pathlib import Path
from collections import defaultdict,Counter
import json,subprocess
N=Path(__file__).resolve().parent;O=N/'current-stacks'
for case in ['all_nodes_property_filter','label_scan_filter']:
    path=O/(case+'.pcs.stacks')
    records=[line.split('\t') for line in path.read_text().splitlines()]
    modules=defaultdict(set)
    for sample,ordinal,frame,module,address in records:modules[module].add(address)
    symbols={}
    for module,addresses in modules.items():
        addresses=sorted(addresses)
        if not Path(module).is_file():
            for a in addresses:symbols[module,a]='unresolved '+module
            continue
        output=subprocess.check_output(['/workspace/zyx-sysroot/usr/bin/llvm-symbolizer-19','--obj='+module,'--output-style=JSON','--no-inlines'],input='\n'.join('0x'+a for a in addresses)+'\n',text=True)
        values=[]
        for line in output.splitlines():
            v=json.loads(line);values.extend(v if isinstance(v,list) else [v])
        assert len(values)==len(addresses)
        for a,v in zip(addresses,values):
            name=(v.get('Symbol') or [{}])[0].get('FunctionName','')
            symbols[module,a]=name if name and name!='??' else 'unresolved '+module
    stacks=defaultdict(list);ordinals={}
    for sample,ordinal,frame,module,address in records:
        stacks[int(sample)].append(symbols[module,address]);ordinals[int(sample)]=int(ordinal)
    inclusive=Counter()
    for stack in stacks.values():inclusive.update(set(stack))
    receipt=json.loads((O/(case+'.receipt.json')).read_text())
    result={'case':case,'quality_passed':receipt['quality_passed'],'independent_stacks':len(stacks),'scope':'Inclusive sample counts overlap; never sum them as exclusive time. Signal unwinding is diagnostic only, can truncate optimized stacks, and does not establish exact CPU shares.','inclusive_samples':inclusive.most_common(),'stacks':[{'sample':i,'query_ordinal':ordinals[i],'frames':frames} for i,frames in sorted(stacks.items())]}
    (O/(case+'.stacks.json')).write_text(json.dumps(result,indent=2)+'\n')
    print(case,'quality',receipt['quality_passed'],'stacks',len(stacks))
    for name,count in inclusive.most_common(22):print(count,name[:170])
