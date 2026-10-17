from pathlib import Path
import subprocess,json,os,signal,sys
sys.path.insert(0,'/workspace/zyx/benchmarks/graphdb-comparison')
from runner.workload_preflight import NATIVE_FAMILY_ORDER,KUZU_FAMILY_ORDER
root=Path('/tmp/zyx-final-five-repaired-window-20261017')
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/run-zyx-final-repaired-write-campaign.py'],check=True)
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
# O6 runs first so an unrelated exhausted family cannot suppress its evidence.
families=['maintenance',*[v for v in NATIVE_FAMILY_ORDER if v!='maintenance']]
registration=json.loads((root/'registration.json').read_text())
os.environ.update(ZYX_FROZEN_BASELINE_WORKER=registration['current_frozen_worker'],ZYX_FROZEN_BASELINE_HARNESS=registration['frozen_harness'])
import importlib
owned_case=importlib.import_module('zyx-final-five-owned-case')
results=[]
for family in families:
 for number,order in enumerate(orders,1):
  for engine in order:
   case=root/'contract'/f'round-{number}'/engine/family
   if engine=='kuzu' and family not in KUZU_FAMILY_ORDER:
    case.mkdir(parents=True,exist_ok=True);entry={'status':'unsupported_NA','engine':engine,'family':family,'round':number};(case/'campaign-case-unsupported.json').write_text(json.dumps(entry));results.append(entry);continue
   marker=next((case/n for n in ['campaign-case-completed.json','campaign-case-failed.json'] if (case/n).exists()),None)
   if marker:results.append(json.loads(marker.read_text()));continue
   previous=next((v for v in results if v['engine']==engine and v['family']==family and v['status'] in ['failed','inconclusive_timeout','unmeasurable_previous_failure']),None)
   if previous:
    case.mkdir(parents=True,exist_ok=True)
    entry={'round':number,'engine':engine,'family':family,'status':'unmeasurable_previous_failure','source':previous,'rule':'Same frozen source and deterministic scenario; no successful sample or protection inferred. Do not repeat an exhausted correctness scenario.'}
    (case/'campaign-case-failed.json').write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);continue
   log=root/'logs'/f'contract-{number}-{engine}-{family}.log'
   print(json.dumps({'phase':'contract-start','round':number,'engine':engine,'family':family}),flush=True)
   timed_out=False
   with log.open('w') as stream:
    process=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-repaired-contract-case.py','--engine',engine,'--round',str(number),'--seed','42','--family',family],stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
    try:process.wait(timeout=1800)
    except subprocess.TimeoutExpired:
     timed_out=True;os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
    except BaseException:
     if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
     raise
   owned_case.cleanup_contract(root,f'round-{number}-{engine}-{family}')
   case.mkdir(parents=True,exist_ok=True)
   entry={'round':number,'engine':engine,'family':family,'status':'inconclusive_timeout' if timed_out else ('completed' if process.returncode==0 else 'failed'),'returncode':process.returncode,'log':str(log),'rule':'Independent family outcome; failures and timeouts are never latency successes or protection passes.'}
   marker='campaign-case-completed.json' if entry['status']=='completed' else 'campaign-case-failed.json'
   (case/marker).write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);print(json.dumps(entry),flush=True)
(root/'contract-campaign-completed.json').write_text(json.dumps({'cases':results,'rounds':3,'status':'all_nonpending_registered_families_attempted_not_all_passed','pending':any(v['status']=='not_run_pending_user_resource_decision' for v in results)},indent=2)+'\n')
