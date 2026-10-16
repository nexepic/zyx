from pathlib import Path
import subprocess,os,time,json
out=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v2');python='/workspace/zyx-benchmark-venv/bin/python';script='/tmp/zyx-final-window.py'
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
cases=[]
for number,order in enumerate(orders,1):
 for scale,profiles in [('medium',['scan','indexed','retrieval']),('holdout',['scan','retrieval'])]:
  for profile in profiles:
   for engine in order:cases.append((number,scale,profile,engine))
for index,(number,scale,profile,engine) in enumerate(cases,1):
 root=out/'reads'/scale/f'round-{number}'/engine/profile
 if list(root.glob('*/correctness.json')):
  print(json.dumps({'phase':'read-already-verified','case':index,'total':len(cases),'root':str(root)}),flush=True)
  continue
 print(json.dumps({'phase':'read-start','case':index,'total':len(cases),'round':number,'scale':scale,'profile':profile,'engine':engine}),flush=True)
 log=out/'logs'/f'read-{number}-{scale}-{profile}-{engine}.log';log.parent.mkdir(exist_ok=True)
 with log.open('w') as stream:
  result=subprocess.run([python,script,'read','--engine',engine,'--round',str(number),'--scale',scale,'--profile',profile],stdout=stream,stderr=subprocess.STDOUT,timeout=1800)
 if result.returncode:raise RuntimeError(f'formal read case failed: {log}')
 print(json.dumps({'phase':'read','completed':index,'total':len(cases),'round':number,'scale':scale,'profile':profile,'engine':engine}),flush=True)
(out/'read-campaign-completed.json').write_text(json.dumps({'cases':len(cases),'rounds':3,'status':'completed'})+'\n')
