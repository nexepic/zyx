from pathlib import Path
import subprocess,json
repo=Path('/workspace/zyx');here=repo/'benchmarks/architecture-design/decisions/20261026-s4-native-write-cost'
for profile in ['candidate','default','compat']:
 r=json.loads((here/(profile+'-recipes.json')).read_text())
 if profile=='candidate':
  cmd=next(c for c in r['compile'] if any(x.endswith('/test_ScanReadProof.cpp') for x in c));subprocess.run(cmd,check=True)
 subprocess.run(r['link'],check=True)
 with (here/(profile+'-final-tests.log')).open('w') as f:subprocess.run([r['link'][-1],'--gtest_output=json:'+str(here/(profile+'-final-tests.json'))],stdout=f,stderr=subprocess.STDOUT,check=True)
 d=json.loads((here/(profile+'-final-tests.json')).read_text());assert d['failures']==0;print(profile,d['tests'],'final module tests passed',flush=True)
