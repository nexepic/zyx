from pathlib import Path
import os,subprocess,tempfile,signal,json,re,time,collections
out=Path('/tmp/zyx-rss-current-attribution')
env={**os.environ,'LD_LIBRARY_PATH':'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu','TMPDIR':tempfile.mkdtemp(prefix='zyx-rss-map-run-'),'ASAN_OPTIONS':'halt_on_error=1:fast_unwind_on_fatal=0','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','ZYX_CI_RSS_TRACE':'1'}
cmd=[str(out/'suite'),'--gtest_filter=RelocatableStorageBackendTest.NativeLogicalReadonlySortSpillsAndPreservesCompleteDescendingCollection','--gtest_output=xml:'+str(out/'maps-result.xml')]
p=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1);reports={}
def snapshot():
 maps=[];item=None
 for line in Path('/proc/'+str(p.pid)+'/smaps').read_text().splitlines():
  if re.match(r'^[0-9a-f]+-[0-9a-f]+ ',line):
   parts=line.split(maxsplit=5);item={'permissions':parts[1],'path':parts[5] if len(parts)>5 else '[anonymous]'};maps.append(item)
  elif item is not None and ':' in line:
   k,v=line.split(':',1)
   if k in ['Rss','Pss','Anonymous','Private_Dirty','Shared_Clean','Private_Clean']:item[k+'_KiB']=int(v.split()[0])
 totals=collections.defaultdict(lambda:collections.Counter())
 for m in maps:
  for key,val in m.items():
   if key.endswith('_KiB'):totals[m['path']][key]+=val
 return {'maps':maps,'by_path':dict(totals)}
try:
 with (out/'maps-run.log').open('w') as f:
  for line in p.stdout:
   f.write(line);f.flush()
   if line.startswith('RSS_MAP_PAUSE phase='):
    phase=line.strip().split('=',1)[1];pid,status=os.waitpid(p.pid,os.WUNTRACED)
    assert os.WIFSTOPPED(status),(pid,status)
    reports[phase]=snapshot();os.kill(p.pid,signal.SIGCONT)
 rc=p.wait(timeout=180)
finally:
 if p.poll() is None:
  os.kill(p.pid,signal.SIGCONT);p.terminate();p.wait(timeout=10)
(out/'mapping-report.json').write_text(json.dumps({'production_source':'561460b3','exit':rc,'snapshots':reports,'note':'Test-only stop at phase boundaries; RSS sampler returns actual unchanged value. Not a performance benchmark.'},indent=2)+'\n')
a=reports['before']['by_path'];b=reports['rejection-before-unwind']['by_path'];changes=[]
for path in set(a)|set(b):
 delta=b.get(path,{}).get('Rss_KiB',0)-a.get(path,{}).get('Rss_KiB',0)
 if delta:changes.append({'path':path,'rss_delta_KiB':delta,'before':a.get(path,{}),'at_rejection':b.get(path,{})})
changes.sort(key=lambda x:x['rss_delta_KiB'],reverse=True);(out/'mapping-delta.json').write_text(json.dumps(changes,indent=2)+'\n')
print('mapping attribution exit',rc)
for x in changes[:12]:print(x['rss_delta_KiB'],x['path'])
