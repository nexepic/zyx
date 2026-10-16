from pathlib import Path
import importlib.util,json,shutil
s=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w);w.restrict()
from runner.workload_preflight import NativeWorker
base=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation')
root=base/'deferred-delete-10000-correctness';root.mkdir(exist_ok=False)
producer=root/'producer';shutil.copy2('/tmp/zyx-resource-diagnostic-worker',producer);producer.chmod(0o555)
source=base/'cache-decline-10000-correctness/private.zyx';shutil.copyfile(source,root/'private.zyx')
(root/'registration.json').write_text(json.dumps({'purpose':'Untimed correctness: one atomic delete of 10000 8KiB nodes, owning 10000 scalar results, unchanged query/resource/deadline caps.','binary_sha256':w.digest(producer),'source_database_sha256':w.digest(source),'query_cap_bytes':64*1024**2,'formal_timing_samples_accepted':0},indent=2)+'\n')
worker=NativeWorker(producer,root/'private.zyx',root)
try:
 worker.call('QUERY','MATCH (n:Churn) RETURN count(*) AS count',expected=[[10000]])
 worker.call('QUERY','MATCH (n:Churn) DELETE n RETURN 1 AS deleted',expected=[[1]]*10000)
 worker.call('QUERY','MATCH (n:Churn) RETURN count(*) AS count',expected=[[0]])
 worker.call('QUERY','MATCH (n:User) RETURN count(*) AS count',expected=[[10000]])
 worker.call('MAINTAIN');worker.call('REOPEN');worker.call('INTEGRITY')
 worker.call('QUERY','MATCH (n:Churn) RETURN count(*) AS count',expected=[[0]])
 stats=worker.call('STATS');(root/'proof.json').write_text(json.dumps({'status':'full_window_correctness_passed','node_deletes':10000,'scalar_results':10000,'unrelated_live_nodes':10000,'reopen_integrity':'passed','stats':stats,'formal_timing_samples_accepted':0},indent=2)+'\n');print(json.dumps(stats),flush=True)
finally:
 (root/'trace.json').write_text(json.dumps(worker.trace,indent=2)+'\n');worker.close()
