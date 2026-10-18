from pathlib import Path
import subprocess,json,os,signal,hashlib
root=Path('/tmp/zyx-final-five-publication-window-20261018')
orders=[['control','current','kuzu'],['current','kuzu','control'],['kuzu','control','current']]
import importlib
owned_case=importlib.import_module('zyx-final-five-owned-case')
results=[]
for scale,profile in [('medium','scan'),('medium','retrieval'),('medium','indexed'),('holdout','scan'),('holdout','retrieval')]:
 for number,order in enumerate(orders,1):
  for engine in order:
   previous=next((v for v in results if v['scale']==scale and v['profile']==profile and v['engine']==engine and v['status']!='completed'),None)
   case=root/'read-case-status'/scale/profile/f'round-{number}'/engine;case.mkdir(parents=True)
   entry={'scale':scale,'profile':profile,'round':number,'engine':engine}
   if previous:entry.update(status='unmeasurable_previous_failure',source=previous)
   else:
    log=root/'logs'/f'read-{scale}-{profile}-{number}-{engine}.log';print(json.dumps({'event':'read-start',**entry}),flush=True)
    command=['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-name-batch-window.py','read','--engine',engine,'--scale',scale,'--profile',profile,'--round',str(number)]
    with log.open('w') as stream:
     process=subprocess.Popen(command,stdout=stream,stderr=subprocess.STDOUT,start_new_session=True);timeout=False
     try:process.wait(timeout=1800)
     except subprocess.TimeoutExpired:timeout=True;os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
     except BaseException:
      if process.poll() is None:os.killpg(process.pid,signal.SIGKILL);process.wait(timeout=5)
      raise
    owned_case.cleanup_read(root,scale,number,engine,profile)
    entry.update(status='inconclusive_timeout' if timeout else ('completed' if process.returncode==0 else 'failed'),returncode=process.returncode,log=str(log))
   (case/'outcome.json').write_text(json.dumps(entry,indent=2)+'\n');results.append(entry);print(json.dumps(entry),flush=True)
(root/'read-campaign-completed.json').write_text(json.dumps({'cases':results,'all_slots_accounted':True,'all_passed':all(v['status']=='completed' for v in results)},indent=2)+'\n')
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-name-batch-bounded-extensions.py'],check=True)
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/report-zyx-final-name-batch-extended-reads.py'],check=True)
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/run-zyx-final-name-batch-write-campaign.py'],check=True)
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/report-zyx-final-name-batch-writes.py'],check=True)
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/zyx-final-name-batch-bounded-write-extensions.py'],check=True)
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/report-zyx-final-name-batch-extended-writes.py'],check=True)
subprocess.run(['/workspace/zyx-benchmark-venv/bin/python','/tmp/run-zyx-final-name-batch-contract-campaign.py','--phase','rest'],check=True)
(root/'integrated-window-completed.json').write_text(json.dumps({'status':'registered_read_and_nonpending_contract_slots_accounted_requires_acceptance_review','resource_gate_review_required':True,'source':'648e60ff'},indent=2)+'\n')
