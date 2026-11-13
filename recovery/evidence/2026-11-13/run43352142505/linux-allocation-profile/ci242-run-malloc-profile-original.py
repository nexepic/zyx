import os, signal, subprocess, time, json
from pathlib import Path

env = os.environ.copy()
env.update(LD_PRELOAD='/workspace/ci242-malloc-profile.so',
           ZYX_ALLOC_PROFILE_OUT='/workspace/ci242-rss-malloc.csv',
           ZYX_CI_RSS_TRACE='1', LLVM_PROFILE_FILE='/workspace/ci242-rss-sample-%p.profraw')
env.pop('ZYX_CI_BUDGET_TRACE', None)
command=['/workspace/ci242-rss-profile-baseline',
         '--gtest_filter=RelocatableStorageBackendTest.NativeLogicalReadonlySortSpillsAndPreservesCompleteDescendingCollection',
         '--gtest_output=xml:/workspace/ci242-rss-profile.xml']
started = time.monotonic()
with open('/workspace/ci242-rss-profile.stdout','wb') as stdout, open('/workspace/ci242-rss-profile.stderr','wb') as stderr:
    process = subprocess.Popen(command, env=env, stdout=stdout, stderr=subprocess.PIPE)
    enabled=False
    for line in process.stderr:
        stderr.write(line)
        if not enabled and b'CI_RSS_PHASE phase=before-query ' in line:
            os.kill(process.pid,signal.SIGUSR2)
            enabled=True
    code=process.wait(timeout=180)
result=dict(exit_code=code, profiler_enabled_after_before_query=enabled, seconds=time.monotonic()-started,
            scope='Linux cumulative allocation request sampling after query start; not live bytes, RSS attribution or native macOS validation')
Path('/workspace/ci242-rss-profile-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
