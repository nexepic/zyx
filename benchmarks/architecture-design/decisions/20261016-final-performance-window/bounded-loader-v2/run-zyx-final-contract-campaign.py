from pathlib import Path
import subprocess,json,os,signal,sys
sys.path.insert(0,'/workspace/zyx/benchmarks/graphdb-comparison')
from runner.workload_preflight import NATIVE_FAMILY_ORDER,KUZU_FAMILY_ORDER
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v2')
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
# O6 runs first so an unrelated exhausted family cannot suppress its evidence.
families=['maintenance',*[v for v in NATIVE_FAMILY_ORDER if v!='maintenance']]
results=[]
for family in families:
 for number,order in enumerate(orders,1):
  for engine in order:
   case=root/'contract'/f'round-{number}'/engine/family
   if engine=='kuzu' and family not in KUZU_FAMILY_ORDER:
    case.mkdir(parents=True,exist_ok=True);entry={'status':'unsupported_NA','engine':engine,'family':family,'round':number};(case/'campaign-case-unsupported.json').write_text(json.dumps(entry));results.append(entry);continue
   marker=next((case/n for n in ['campaign-case-completed.json','campaign-case-failed.json'] if (case/n).exists()),None)
   if marker:results.append(json.loads(marker.read_text()));continue
   log=root/'logs'/f'contract-{number}-{engine}-{family}.log'
   print(json.dumps({'phase':'contract-start','round':number,'engine':engine,'family':family}),flush=True)
   timed_out=False
   with log.open('w') as stream:
    process=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-contract-case.py','--engine',engine,'--round',str(number),'--seed','42','--family',family],stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
    try:process.wait(timeout=1800)
    except subprocess.TimeoutExpired:
     timed_out=True;os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
    except BaseException:
     if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
     raise
   case.mkdir(parents=True,exist_ok=True)
   entry={'round':number,'engine':engine,'family':family,'status':'inconclusive_timeout' if timed_out else ('completed' if process.returncode==0 else 'failed'),'returncode':process.returncode,'log':str(log),'rule':'Independent family outcome; failures and timeouts are never latency successes or protection passes.'}
   marker='campaign-case-completed.json' if entry['status']=='completed' else 'campaign-case-failed.json'
   (case/marker).write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);print(json.dumps(entry),flush=True)
(root/'contract-campaign-completed.json').write_text(json.dumps({'cases':results,'rounds':3,'status':'all_registered_families_attempted_not_all_passed'},indent=2)+'\n')
