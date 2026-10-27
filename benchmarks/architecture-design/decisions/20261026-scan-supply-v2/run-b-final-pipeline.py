"""Wait for immutable offline construction, then run gated final validation.
Waiting creates no database and launches no repeated performance windows.
"""
from pathlib import Path
import json,subprocess,sys,time
D=Path(__file__).resolve().parent;B=D/'b-field-cohort'
def wait_for(p):
 deadline=time.monotonic()+21600
 while not p.exists():
  if time.monotonic()>deadline:raise TimeoutError(str(p))
  time.sleep(1)
 return json.loads(p.read_text())
print('Awaiting immutable real-data conversion receipts',flush=True)
for name in ['medium','holdout']:
 c=wait_for(B/(name+'-conversion-receipt.json'))
 assert c['resources']['returncode']==0,(name,c)
 if name=='medium':
  assert not (B/'medium-validation').exists(),'Preserve any prior validation attempt'
  subprocess.run([sys.executable,str(D/'check-b-field-cohort.py')],check=True)
print('Both conversions and untimed medium validation complete',flush=True)
assert json.loads((B/'medium-validation/receipt.json').read_text())['work_gate_passed']
subprocess.run([sys.executable,str(D/'final-primary-b.py'),'--validated-run'],check=True)
g=json.loads((B/'final-primary/gates.json').read_text());print('PRIMARY GATES',g,flush=True)
if g['gain_gate_passed']:
 subprocess.run([sys.executable,str(D/'final-guards-b.py'),'--validated-run'],check=True)
 subprocess.run([sys.executable,str(D/'check-write-costs-b.py')],check=True)
 print('Adoption review required; no automatic progress acceptance',flush=True)
else:print('Primary gain failed; preserved; no full guard matrix launched',flush=True)
