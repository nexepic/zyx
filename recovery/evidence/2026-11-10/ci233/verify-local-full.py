import collections,gzip,importlib.util,json,re,xml.etree.ElementTree as ET
from pathlib import Path
root=Path('/workspace/zyx-ci-reconstructed');ev=root/'recovery/evidence/2026-11-10/ci233'
suite='';discovered=[]
for line in Path('/tmp/zyx-233-discovery.txt').read_text().splitlines():
 line=line.split('#',1)[0].rstrip()
 if line and not line[0].isspace() and line.endswith('.'):suite=line
 elif line.startswith('  '):discovered.append(suite+line.strip())
def disabled(s):return any(p.startswith('DISABLED_') for p in s.replace('.','/').split('/'))
enabled={s for s in discovered if not disabled(s)};executed=[];failures=[];skips=[];times=[]
for p in sorted(Path('/tmp/zyx-233-complete-xml').glob('shard-*.xml')):
 tree=ET.parse(p);times.append(float(tree.getroot().get('time')))
 for c in tree.iter('testcase'):
  if c.get('status')!='run':continue
  name=c.get('classname')+'.'+c.get('name');executed.append(name)
  if c.find('failure') is not None:failures.append(name)
  if c.find('skipped') is not None or c.get('result')=='skipped':skips.append(name)
 (ev/(p.name+'.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
counter=collections.Counter(executed);begins=[];ends=[];tracefail=[]
for p in sorted((root/'buildDir/coverage/runs/ci233-complete-8f9c5fe1d/failed-shards').glob('shard-*.log')):
 s=p.read_text(errors='replace');begins+=re.findall(r'CI_CASE_BEGIN (\S+)',s)
 for n,bad in re.findall(r'CI_CASE_END (\S+) failed=(\d)',s):
  ends.append(n)
  if bad!='0':tracefail.append(n)
 (ev/(p.name+'.gz')).write_bytes(gzip.compress(p.read_bytes(),mtime=0))
sp=importlib.util.spec_from_file_location('tier',root/'scripts/run_test_tier.py');tier=importlib.util.module_from_spec(sp);sp.loader.exec_module(tier)
r={'code_commit':'8f9c5fe1dbc625e7b465e702130b7897de22c8a8','discovered':len(discovered),'enabled':len(enabled),'disabled':sorted(s for s in discovered if disabled(s)),'executed':len(executed),'distinct':len(counter),'failures':failures,'skips':skips,'missing':sorted(enabled-set(counter)),'extra':sorted(set(counter)-enabled),'duplicates':{n:c for n,c in counter.items() if c!=1},'begin_count':len(begins),'end_count':len(ends),'trace_matches':collections.Counter(begins)==counter==collections.Counter(ends),'trace_failures':tracefail,'stress_counts':{s:counter[s] for s in tier.STRESS_TESTS},'shard_times_seconds':times}
assert len(discovered)==len(set(discovered))==11090 and len(enabled)==11070
assert not any(r[k] for k in ('failures','skips','missing','extra','duplicates','trace_failures')) and r['trace_matches']
assert all(c==1 for c in r['stress_counts'].values())
(ev/'full-summary.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
