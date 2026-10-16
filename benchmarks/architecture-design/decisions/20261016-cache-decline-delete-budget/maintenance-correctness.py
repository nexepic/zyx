from pathlib import Path
import importlib.util,json,shutil,os
s=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
from runner.workload_preflight import NativeWorker
base=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation');root=base/'deferred-delete-maint-lifetime-v2';root.mkdir(exist_ok=False)
shutil.copyfile(base/'deferred-delete-10000-correctness/private.zyx',root/'private.zyx')
os.environ['LD_PRELOAD']='/tmp/zyx-maint-admission-raw-stack.so'
w=NativeWorker(Path('/tmp/zyx-resource-diagnostic-worker'),root/'private.zyx',root)
try:
 print(w.call('STATS'),flush=True);print(w.call('MAINTAIN'),flush=True);w.call('REOPEN');w.call('INTEGRITY');w.call('QUERY','MATCH (n:Churn) RETURN count(*) AS count',expected=[[0]]);w.call('QUERY','MATCH (n:User) RETURN count(*) AS count',expected=[[10000]]);stats=w.call('STATS');print(stats,flush=True);(root/'proof.json').write_text(json.dumps({'status':'maintenance_reopen_integrity_passed','stats':stats,'formal_timing_samples_accepted':0},indent=2)+'\n')
finally:
 (root/'trace.json').write_text(json.dumps(w.trace,indent=2)+'\n');w.close()
