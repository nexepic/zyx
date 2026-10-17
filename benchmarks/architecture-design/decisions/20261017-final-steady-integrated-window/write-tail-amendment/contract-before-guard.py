from pathlib import Path
import subprocess,json,os,signal,sys,argparse
sys.path.insert(0,'/workspace/zyx/benchmarks/graphdb-comparison')
from runner.workload_preflight import NATIVE_FAMILY_ORDER,KUZU_FAMILY_ORDER
root=Path('/tmp/zyx-final-five-steady-window-20261017')
p=argparse.ArgumentParser();p.add_argument('--phase',choices=['maintenance','rest'],required=True);phase=p.parse_args().phase
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
# O6 runs first so an unrelated exhausted family cannot suppress its evidence.
families=['maintenance'] if phase=='maintenance' else [v for v in NATIVE_FAMILY_ORDER if v!='maintenance']
registration=json.loads((root/'registration.json').read_text())
os.environ.update(ZYX_FROZEN_BASELINE_WORKER=registration['current_frozen_worker'],ZYX_FROZEN_BASELINE_HARNESS=registration['frozen_harness'])
import importlib
owned_case=importlib.import_module('zyx-final-five-owned-case')
results=[]
for family in families:
 for number,order in enumerate(registration['maintenance_orders'] if family=='maintenance' else orders,1):
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
    process=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-steady-contract-case.py','--engine',engine,'--round',str(number),'--seed','42','--family',family],stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
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
   if family=='maintenance' and engine=='current':
    checkpoint={'engine':engine,'round':number,'case_status':entry['status'],'automatic_acceptance':False}
    if entry['status']=='completed':
     report=json.loads((case/'report.json').read_text());windows=report['maintenance_windows'];assert len(windows)==10 and report['full_results_checked']
     growth=windows[-1]['file_bytes']/windows[0]['file_bytes']-1
     checkpoint.update(first_file_bytes=windows[0]['file_bytes'],last_file_bytes=windows[-1]['file_bytes'],file_growth_fraction=growth,full13window_correctness='passed',full_gate_review_pending=True)
     checkpoint['resource_checkpoint']='within_original5percent_growth_boundary' if growth<=.05 else 'failed_original_resource_growth_boundary'
    else:checkpoint['resource_checkpoint']='failed_or_inconclusive_correctness'
    (case/'resource-checkpoint.json').write_text(json.dumps(checkpoint,indent=2)+'\n')
    if checkpoint['resource_checkpoint']!='within_original5percent_growth_boundary':
     (root/'stopped-after-actual-O6-checkpoint.json').write_text(json.dumps(checkpoint,indent=2)+'\n')
     raise SystemExit('Stop candidate after actualO6failure; broad matrix not started')

(root/('maintenance-campaign-completed.json' if phase=='maintenance' else 'remaining-contract-campaign-completed.json')).write_text(json.dumps({'cases':results,'rounds':3,'status':'all_nonpending_registered_families_attempted_not_all_passed','pending':any(v['status']=='not_run_pending_user_resource_decision' for v in results)},indent=2)+'\n')
