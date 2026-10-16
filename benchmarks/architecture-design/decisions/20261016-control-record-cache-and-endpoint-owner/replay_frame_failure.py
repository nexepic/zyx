from pathlib import Path
import sys,json,importlib.util
spec=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w);w.restrict()
from runner.workload_preflight import NativeWorker
source=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/current-final-virtual-child-smoke');trace=json.loads((source/'partial-trace.json').read_text())
def restore(value):
 if isinstance(value,dict) and 'utf8_blob' in value:return (source/value['utf8_blob']).read_text()
 if isinstance(value,list):return [restore(v) for v in value]
 if isinstance(value,dict):return {k:restore(v) for k,v in value.items()}
 return value
for number in range(1,9):
 folder=source.parent/f'frame-replay-final-{number}';folder.mkdir();worker=NativeWorker(w.CURRENT/'zyx-workload-contract-worker',folder/'private.zyx',folder)
 try:
  for case in trace:worker.call(*restore(case['command']),expected=restore(case['expected']),ordered=case['ordered'],error_contains=case['expected_error_contains'])
  worker.call('MIXED_THREADS','1000');worker.call('INTEGRITY');print(json.dumps({'case':number,'passed':True,'operations':len(worker.trace)}),flush=True)
 except Exception as error:print(json.dumps({'case':number,'passed':False,'error':str(error)}),flush=True);break
 finally:
  (folder/'trace.json').write_text(json.dumps(worker.trace,indent=2)+'\n');worker.close()
