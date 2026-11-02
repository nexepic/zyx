import json,pathlib,re,statistics,subprocess,tempfile
from math import sqrt
root=pathlib.Path('/workspace/zyx-link-tmp/fair-formal');root.mkdir(exist_ok=False)
results=[]
for iteration in range(12):
 for variant in (('control','enabled') if iteration%2==0 else ('enabled','control')):
  with tempfile.TemporaryDirectory(prefix='db-',dir=root) as td:
   p=subprocess.run(['taskset','-c','0',f'/workspace/zyx-link-tmp/fair-client-{variant}',str(pathlib.Path(td)/'data.zyx')],text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   log=root/f'round{iteration+1:02}-{variant}.log';log.write_text(f'exit={p.returncode}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}')
   fields=dict(re.findall(r'(\w+)=([^\s]+)',p.stdout))
   if p.returncode or fields.get('failures')!='0' or fields.get('iterations')!='128':raise SystemExit(f'failed: {log}')
   results.append({'round':iteration+1,'variant':variant,**fields});(root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
for metric in ('pairs_per_s','delete_mean_ms','delete_p95_ms','file_bytes'):
 by={(r['round'],r['variant']):float(r[metric]) for r in results}
 diffs=[by[(n,'enabled')]-by[(n,'control')] for n in range(1,13)]
 mean=statistics.mean(diffs);half=2.2009851601*statistics.stdev(diffs)/sqrt(12)
 print(f'metric={metric} control={statistics.mean(by[(n,"control")] for n in range(1,13)):.4f} enabled={statistics.mean(by[(n,"enabled")] for n in range(1,13)):.4f} delta={mean:.4f} CI95=[{mean-half:.4f},{mean+half:.4f}]')
