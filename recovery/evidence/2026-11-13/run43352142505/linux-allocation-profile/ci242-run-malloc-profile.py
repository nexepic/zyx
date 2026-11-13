import os, signal, subprocess, time, json, argparse, threading
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--binary',default='/workspace/ci242-rss-profile-baseline')
parser.add_argument('--prefix',default='/workspace/ci242-rss-profile-after')
args=parser.parse_args()
env = os.environ.copy()
env.update(LD_PRELOAD='/workspace/ci242-malloc-profile.so',
           ZYX_ALLOC_PROFILE_OUT=args.prefix+'-malloc.csv',
           ZYX_CI_RSS_TRACE='1', LLVM_PROFILE_FILE='/workspace/ci242-rss-sample-%p.profraw')
env.pop('ZYX_CI_BUDGET_TRACE', None)
command=[args.binary,
         '--gtest_filter=RelocatableStorageBackendTest.NativeLogicalReadonlySortSpillsAndPreservesCompleteDescendingCollection',
         '--gtest_output=xml:'+args.prefix+'.xml']
started = time.monotonic()
timed_out=[False]
with open(args.prefix+'.stdout','wb') as stdout, open(args.prefix+'.stderr','wb') as stderr:
    process = subprocess.Popen(command, env=env, stdout=stdout, stderr=subprocess.PIPE,start_new_session=True)
    def expire():
        timed_out[0]=True
        try:os.killpg(process.pid,signal.SIGTERM)
        except ProcessLookupError:return
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:pass
    timer=threading.Timer(180,expire);timer.daemon=True;timer.start()
    enabled=False
    for line in process.stderr:
        stderr.write(line)
        if not enabled and b'CI_RSS_PHASE phase=before-query ' in line:
            os.kill(process.pid,signal.SIGUSR2)
            enabled=True
    code=process.wait(timeout=5);timer.cancel()
result=dict(exit_code=124 if timed_out[0] else code, timed_out=timed_out[0], timeout_seconds=180,
            profiler_enabled_after_before_query=enabled, seconds=time.monotonic()-started,
            scope='Linux cumulative allocation request sampling after query start; not live bytes, RSS attribution or native macOS validation')
Path(args.prefix+'-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
