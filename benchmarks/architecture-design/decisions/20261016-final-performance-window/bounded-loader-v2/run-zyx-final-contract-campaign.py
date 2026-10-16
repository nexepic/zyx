from pathlib import Path
import subprocess,json,os,signal
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v2')
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
for number,order in enumerate(orders,1):
 for engine in order:
  case=root/'contract'/f'round-{number}'/engine
  if (case/'campaign-case-completed.json').exists():continue
  log=root/'logs'/f'contract-{number}-{engine}.log'
  print(json.dumps({'phase':'contract-start','round':number,'engine':engine}),flush=True)
  with log.open('w') as stream:
   result=subprocess.Popen(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-contract-case.py','--engine',engine,'--round',str(number),'--seed','42'],stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
   try:result.wait(timeout=1800)
   except BaseException:
    try:os.killpg(result.pid,signal.SIGKILL)
    except ProcessLookupError:pass
    result.wait(timeout=5)
    raise
  if result.returncode:raise RuntimeError(f'contract case failed: {log}')
  (case/'campaign-case-completed.json').write_text(json.dumps({'round':number,'engine':engine,'status':'completed'})+'\n')
  print(json.dumps({'phase':'contract-completed','round':number,'engine':engine}),flush=True)
(root/'contract-campaign-completed.json').write_text(json.dumps({'cases':9,'rounds':3,'status':'completed'})+'\n')
