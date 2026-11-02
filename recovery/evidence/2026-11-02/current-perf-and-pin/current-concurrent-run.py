import pathlib,subprocess,tempfile,re,json,statistics
r=pathlib.Path('/workspace/zyx-link-tmp/current-concurrent-evidence');r.mkdir(exist_ok=False);rows=[]
for n in (2,4):
 for i in range(1,7):
  with tempfile.TemporaryDirectory(prefix='database-',dir=r) as d:
   cmd=['taskset','-c','0-3','/workspace/zyx-link-tmp/concurrent-current',str(n),str(pathlib.Path(d)/'test.zyx')]
   p=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
   (r/f'clients{n}-round{i}.log').write_text(f'command={cmd!r}\nexit={p.returncode}\n{p.stdout}\n{p.stderr}')
   f=dict(re.findall(r'(\w+)=([^\s]+)',p.stdout))
   if p.returncode or f.get('failures')!='0' or f.get('operations')!=str(100*n):raise SystemExit(f'failure {n}/{i}')
   rows.append({'clients':n,'round':i,**f});(r/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
for n in (2,4):
 s=[x for x in rows if int(x['clients'])==n]
 for k in ('throughput','commit_mean_ms','commit_p95_ms','conflicts','retries'):
  vals=[float(x[k]) for x in s];print('clients',n,k,'mean',round(statistics.mean(vals),4),'range',min(vals),max(vals))
