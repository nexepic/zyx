from pathlib import Path
import subprocess,os,time,json,signal
out=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v3');python='/workspace/zyx-benchmark-venv/bin/python';script='/tmp/zyx-final-window.py'
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
cases=[]
for number,order in enumerate(orders,1):
 for scale,profiles in [('medium',['scan','indexed','retrieval']),('holdout',['scan','retrieval'])]:
  for profile in profiles:
   for engine in order:cases.append((number,scale,profile,engine))
for index,(number,scale,profile,engine) in enumerate(cases,1):
 root=out/'reads'/scale/f'round-{number}'/engine/profile
 failure=Path('/workspace/zyx-optimization-final-window-20261016/prepared-inputs')/scale/f'{engine}-preparation-failure.json'
 if failure.exists():
  assert engine=='control';root.mkdir(parents=True,exist_ok=True)
  (root/'preparation-failure.json').write_text(failure.read_text())
  print(json.dumps({'phase':'read-unmeasurable-frozen-control','case':index,'scale':scale,'profile':profile,'engine':engine,'failure':str(failure)}),flush=True)
  continue
 prior=next((v for v in (out/'reads'/scale).glob(f'round-*/{engine}/{profile}/campaign-case-failed.json')),None)
 if prior:
  root.mkdir(parents=True,exist_ok=True)
  target=root/'campaign-case-failed.json'
  if not target.exists():target.write_text(json.dumps({'status':'unmeasurable_previous_failure','source':str(prior),'rule':'Same frozen binary and deterministic profile; no successful process or latency ratio inferred.'})+'\n')
  print(json.dumps({'phase':'read-unmeasurable-previous-failure','case':index,'source':str(prior)}),flush=True);continue
 verified=list(root.glob('*/correctness.json'))
 if verified and any(json.loads((v.parent/'run_status.json').read_text())=={'failure_count':0,'quality_failure_count':0} for v in verified):

  print(json.dumps({'phase':'read-already-verified','case':index,'total':len(cases),'root':str(root)}),flush=True)
  continue
 print(json.dumps({'phase':'read-start','case':index,'total':len(cases),'round':number,'scale':scale,'profile':profile,'engine':engine}),flush=True)
 log=out/'logs'/f'read-{number}-{scale}-{profile}-{engine}.log';log.parent.mkdir(exist_ok=True)
 with log.open('w') as stream:
  result=subprocess.Popen([python,script,'read','--engine',engine,'--round',str(number),'--scale',scale,'--profile',profile],stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
  try:result.wait(timeout=1800)
  except BaseException:
   try:os.killpg(result.pid,signal.SIGKILL)
   except ProcessLookupError:pass
   result.wait(timeout=5)
   raise
 if result.returncode:
  root.mkdir(parents=True,exist_ok=True)
  (root/'campaign-case-failed.json').write_text(json.dumps({'status':'failed','returncode':result.returncode,'log':str(log),'rule':'Preserve exact process output and original engine failure. No successful profile process or latency ratio inferred; continue independent scenarios.'},indent=2)+'\n')
  print(json.dumps({'phase':'read-failed','case':index,'engine':engine,'profile':profile,'log':str(log)}),flush=True);continue
 print(json.dumps({'phase':'read','completed':index,'total':len(cases),'round':number,'scale':scale,'profile':profile,'engine':engine}),flush=True)
(out/'read-campaign-completed.json').write_text(json.dumps({'cases':len(cases),'rounds':3,'status':'completed'})+'\n')
