from pathlib import Path
import os,subprocess,tempfile,threading,time,re,json
p=Path('/tmp/zyx-rss-current-attribution');env={**os.environ,'LD_LIBRARY_PATH':'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu','TMPDIR':tempfile.mkdtemp(prefix='zyx-normal-rss-sample-'),'LLVM_PROFILE_FILE':str(p/'normal-sampled-%p.profraw'),'ZYX_CI_RSS_TRACE':'1'}
cmd=['/workspace/zyx-ci-config/zyx_test_suite','--gtest_filter=RelocatableStorageBackendTest.NativeLogicalReadonlySortSpillsAndPreservesCompleteDescendingCollection']
child=subprocess.Popen(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,bufsize=1);baseline=None;peak=0;samples=0;done=False
page=os.sysconf('SC_PAGESIZE')
def sample():
 global peak,samples
 while not done:
  if baseline is not None:
   try:
    rss=int(Path('/proc/'+str(child.pid)+'/statm').read_text().split()[1])*page;peak=max(peak,rss);samples+=1
   except FileNotFoundError:pass
  time.sleep(.005)
t=threading.Thread(target=sample);t.start()
try:
 with (p/'normal-sampled.log').open('w') as f:
  for line in child.stdout:
   f.write(line)
   if line.startswith('CI_RSS_BEGIN'):baseline=int(re.search(r'rss=(\d+)',line)[1]);peak=baseline
 rc=child.wait(timeout=180)
finally:done=True;t.join()
r={'source_commit':'561460b3','build':'normal DebugO0 coverage; no ASan/UBSan','exit':rc,'reported_before_query_rss':baseline,'external_observed_peak_after_marker':peak,'observed_growth':peak-baseline,'sampling_interval_seconds':.005,'samples':samples,'limitations':'Sampled lower bound on peak; includes end-of-test cleanup and profile flush, not exact allocation accounting or comparable-instrumentation speed benchmark.'};(p/'normal-sampled.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r,indent=2))
