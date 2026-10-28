import json,pathlib,statistics
here=pathlib.Path(__file__).resolve().parent
r=json.loads((here/'persistent-receipt.json').read_text()); runs=r['runs']
assert len(runs)==60 and all(x['exit']==0 and len(x['samples'])==11 for x in runs)
out=[]
for rows in (512,1536):
 for mode in range(5):
  arms={a:[x for x in runs if x['rows']==rows and x['mode']==mode and x['arm']==a] for a in (0,1)}
  assert all(len(arms[a])==3 for a in (0,1))
  med={}; rss={}; proc={}
  for a in (0,1):
   values=[]; ps=[]
   for x in sorted(arms[a],key=lambda y:y['process']):
    v=[int(dict(s.split('=') for s in line.split())['ns']) for line in x['samples'][3:]]
    values.extend(v); ps.append(statistics.median(v))
   med[a]=statistics.median(values);proc[a]=ps;rss[a]=[x['rss_kib_process'] for x in arms[a]]
  item={'rows':rows,'mode':mode,'control_ns_median':med[0],'candidate_ns_median':med[1],
        'control_over_candidate':med[0]/med[1], 'process_ratios':[proc[0][i]/proc[1][i] for i in range(3)],
        'process_medians_ns':{'control':proc[0],'candidate':proc[1]}, 'process_peak_rss_kib':{'control':rss[0],'candidate':rss[1]},'samples_per_arm':24}
  out.append(item)
(here/'persistent-summary.json').write_text(json.dumps({'scope':'synthetic warm reopened SCAN; includes complete result consumption/oracle; no query-level allocation or P95 claim','scenarios':out},indent=2)+'\n')
for x in out: print(x['rows'],x['mode'],'ratio',round(x['control_over_candidate'],3),'process',*[round(y,3) for y in x['process_ratios']],'rss',x['process_peak_rss_kib'])
