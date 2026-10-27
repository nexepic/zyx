from pathlib import Path
from collections import defaultdict,Counter
import json,subprocess
D=Path(__file__).resolve().parent;summary={}
for case in ['all_nodes_property_filter','label_scan_filter']:
 windows=[]
 for line in (D/('allocation-'+case+'.stacks')).read_text().splitlines():
  if line.startswith('# window '):windows.append({'header':line,'stacks':defaultdict(list)})
  elif line.startswith('# size_bins'):windows[-1]['size_bins']=line
  elif not line.startswith('#'):
   index,size,frame,module,address=line.split('\t');windows[-1]['stacks'][int(index)].append((int(frame),module,int(address,16)-1,int(size)))
 window=windows[-1];groups=defaultdict(set)
 for frames in window['stacks'].values():
  for _,module,address,_ in frames:groups[module].add(address)
 names={}
 for module,addresses in groups.items():
  path=Path('/tmp/zyx-libc-symbols/usr/lib/debug/.build-id/5e/5b51f4c39af9ac589408c1dc432ba390a273cc.debug') if module=='/lib/x86_64-linux-gnu/libc.so.6' else Path(module)
  ordered=sorted(addresses)
  if not path.is_file():continue
  p=subprocess.run(['/workspace/zyx-sysroot/usr/bin/llvm-symbolizer-19','--obj='+str(path),'--output-style=JSON','--no-inlines'],input='\n'.join(hex(x) for x in ordered)+'\n',text=True,capture_output=True,check=True)
  values=[]
  for line in p.stdout.splitlines():
   if line.strip():
    value=json.loads(line);values.extend(value if isinstance(value,list) else [value])
  assert len(values)==len(ordered)
  for address,value in zip(ordered,values):names[module,address]=(value.get('Symbol') or [{}])[0].get('FunctionName','unknown')
 inclusive=Counter();domain=Counter();resolved=[]
 for index,frames in window['stacks'].items():
  chain=[names.get((module,address),'unresolved '+module) for _,module,address,_ in sorted(frames)]
  inclusive.update(set(chain))
  caller=next((name for name in chain if name.startswith(('graph::storage::','graph::query::')) and 'ResourceCoordinator' not in name and 'ResourceLease' not in name),'no domain frame in 20-frame sample')
  domain[caller]+=1;resolved.append({'sample':index,'requested_bytes':frames[0][3],'frames':chain})
 summary[case]={'header':window['header'],'size_bins':window['size_bins'],'sampled_stacks':len(window['stacks']),'domain_callers':domain.most_common(),'inclusive_frames_non_additive':inclusive.most_common(),'latency_usable':False}
 (D/('allocation-'+case+'-resolved.json')).write_text(json.dumps(resolved,indent=2)+'\n')
 print(case,window['header'])
 for name,count in domain.most_common(12):print(count,name)
(D/'allocation-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
