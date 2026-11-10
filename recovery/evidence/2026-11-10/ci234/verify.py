import collections,gzip,importlib.util,json,subprocess,xml.etree.ElementTree as ET
from pathlib import Path
root=Path('/workspace/zyx-ci-reconstructed');ev=root/'recovery/evidence/2026-11-10/ci234'
suite='';discovered=[]
for line in Path('/tmp/zyx-234-discovery.txt').read_text().splitlines():
 line=line.split('#',1)[0].rstrip()
 if line and not line[0].isspace() and line.endswith('.'):suite=line
 elif line.startswith('  '):discovered.append(suite+line.strip())
def disabled(s):return any(p.startswith('DISABLED_') for p in s.replace('.','/').split('/'))
enabled={s for s in discovered if not disabled(s)};executed=[];failures=[];skips=[];times=[]
paths=sorted(Path('/tmp/zyx-234-full-xml').glob('shard-*.xml'));assert len(paths)==4
for p in paths:
 tree=ET.parse(p);times.append(float(tree.getroot().get('time')))
 for c in tree.iter('testcase'):
  if c.get('status')!='run':continue
  name=c.get('classname')+'.'+c.get('name');executed.append(name)
  if c.find('failure') is not None or c.find('error') is not None:failures.append(name)
  if c.find('skipped') is not None or c.get('result')=='skipped':skips.append(name)
 (ev/(p.name+'.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
counter=collections.Counter(executed)
sp=importlib.util.spec_from_file_location('tier',root/'scripts/run_test_tier.py');tier=importlib.util.module_from_spec(sp);sp.loader.exec_module(tier)
r={'code_commit':'0fe25fbd081f48f2b1fb7f7cd94f054a507d1996','discovered':len(discovered),'enabled':len(enabled),'disabled':sorted(s for s in discovered if disabled(s)),'executed':len(executed),'distinct':len(counter),'failures':failures,'skips':skips,'missing':sorted(enabled-set(counter)),'extra':sorted(set(counter)-enabled),'duplicates':{n:c for n,c in counter.items() if c!=1},'stress_counts':{s:counter[s] for s in tier.STRESS_TESTS},'shard_times_seconds':times,'evidence_basis':'discovery plus all four XML files; no case-trace assertion'}
assert len(discovered)==len(set(discovered))
assert not any(r[k] for k in ('failures','skips','missing','extra','duplicates'))
assert all(c==1 for c in r['stress_counts'].values())
(ev/'full-summary.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
