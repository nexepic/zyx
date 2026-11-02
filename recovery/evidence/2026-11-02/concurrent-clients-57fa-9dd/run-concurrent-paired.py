import json,os,pathlib,re,statistics,subprocess,tempfile,time
from math import sqrt
root=pathlib.Path('/workspace/zyx-link-tmp/concurrent-formal'); root.mkdir(exist_ok=False)
result=[]
for clients in (2,4):
 for iteration in range(6):
  versions=('57fa','9dd') if iteration%2==0 else ('9dd','57fa')
  for version in versions:
   with tempfile.TemporaryDirectory(prefix='db-',dir=root) as td:
    cmd=['taskset','-c','0-3',f'/workspace/zyx-link-tmp/concurrent-client-{version}',str(clients),str(pathlib.Path(td)/'data.zyx')]
    p=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    log=root/f'c{clients}-round{iteration+1}-{version}.log'
    log.write_text(f'exit={p.returncode}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}')
    fields=dict(re.findall(r'(\w+)=([^\s]+)',p.stdout))
    if p.returncode or fields.get('failures')!='0' or fields.get('operations')!=str(clients*100):
     raise SystemExit(f'failed, see {log}')
    result.append({'clients':clients,'round':iteration+1,'version':version,**fields})
    (root/'results.json').write_text(json.dumps(result,indent=2)+'\n')
for clients in (2,4):
 for metric in ('throughput','commit_mean_ms','commit_p95_ms'):
  by={(x['round'],x['version']):float(x[metric]) for x in result if int(x['clients'])==clients}
  diffs=[by[(n,'9dd')]-by[(n,'57fa')] for n in range(1,7)]
  mean=statistics.mean(diffs);half=2.5705818366*statistics.stdev(diffs)/sqrt(6)
  print(f'clients={clients} metric={metric} 57fa={statistics.mean(by[(n,"57fa")] for n in range(1,7)):.4f} 9dd={statistics.mean(by[(n,"9dd")] for n in range(1,7)):.4f} delta={mean:.4f} CI95=[{mean-half:.4f},{mean+half:.4f}]')
