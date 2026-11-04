from pathlib import Path
import xml.etree.ElementTree as E,collections,json
all_names=[];suite=''
for line in Path('/tmp/zyx-projection-full-discovery.txt').read_text().splitlines():
 text=line.split('#',1)[0].strip()
 if not text:continue
 if not line.startswith(' '):suite=text
 else:all_names.append(suite+text)
disabled={n for n in all_names if any(p.startswith('DISABLED_') for p in n.split('.'))};enabled=set(all_names)-disabled
counts=collections.Counter();fail=[];skip=[]
paths=list(Path('/tmp/zyx-projection-full-unit-xml').glob('*.xml'))+list(Path('/tmp/zyx-projection-full-stress-xml').glob('*.xml'))
for p in paths:
 for c in E.parse(p).iter('testcase'):
  n=c.get('classname')+'.'+c.get('name')
  if c.get('status')=='run':counts[n]+=1
  if c.find('failure') is not None:fail.append(n)
  if c.find('skipped') is not None:skip.append(n)
result=dict(source='dd8dbca0',discovered_enabled=len(enabled),disabled=len(disabled),executed_distinct=len(counts),xml_files=len(paths),missing=sorted(enabled-counts.keys()),extra=sorted(counts.keys()-enabled),duplicates=[n for n,c in counts.items() if c!=1],failures=fail,skips=skip,disabled_names=sorted(disabled))
Path('/tmp/zyx-projection-full-exact-once.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k!='disabled_names'},indent=2))
assert not any(result[k] for k in ('missing','extra','duplicates','failures','skips'))
