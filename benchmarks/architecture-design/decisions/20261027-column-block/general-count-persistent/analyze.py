#!/usr/bin/env python3
import collections, hashlib, json, pathlib, statistics
p=pathlib.Path(__file__).resolve().parent
receipt=json.loads((p/'receipt.json').read_text())
assert receipt['protocol_sha256']==hashlib.sha256((p/'protocol.json').read_bytes()).hexdigest()
assert len(receipt['runs'])==12 and all(x['exit_code']==0 for x in receipt['runs'])
cases=collections.defaultdict(lambda: collections.defaultdict(list)); work=[]
for run in receipt['runs']:
 for line in (p/run['file']).read_text().splitlines():
  if line.startswith('PERSIST_PROBE '):
   d=dict(v.split('=',1) for v in line.split()[1:]); key=(int(d['rows']),int(d['mixed']),int(d['threshold']),int(d['expected']))
   cases[key][(run['process'],int(d['enabled']))].append(int(d['ns']))
  if line.startswith('PERSIST_WORK '): work.append(dict(v.split('=',1) for v in line.split()[1:]))
assert len(cases)==12 and len(work)==72
for w in work:
 assert (int(w['stream'])==int(w['rows']) and int(w['direct'])==0) if w['enabled']=='1' else (int(w['stream'])==0 and int(w['direct'])==int(w['rows']))
out=[]
for (rows,mixed,threshold,expected),arms in sorted(cases.items()):
 assert all(len(arms[(i,a)])==6 for i in range(3) for a in (0,1))
 on=[v for i in range(3) for v in arms[(i,1)]]; off=[v for i in range(3) for v in arms[(i,0)]]
 out.append({'rows':rows,'mixed':bool(mixed),'threshold':threshold,'matches':expected,
  'fusion_median_ns':statistics.median(on),'control_median_ns':statistics.median(off),
  'pooled_speedup':statistics.median(off)/statistics.median(on),
  'process_speedups':[statistics.median(arms[(i,0)])/statistics.median(arms[(i,1)]) for i in range(3)],
  'fusion_range_ns':[min(on),max(on)],'control_range_ns':[min(off),max(off)]})
(p/'summary.json').write_text(json.dumps({'protocol_sha256':receipt['protocol_sha256'],'binary_sha256':receipt['binary_sha256'],
 'cases':out,'work_observations':len(work),'allocation_metrics':'not instrumented',
 'rss_scope':'whole process including fixture building and reopen; not per query',
 'limitations':'small generated warm persistent SCAN fixture, three processes, six samples per arm; not formal or production'},indent=2)+'\n')
for c in out: print(c['rows'],c['mixed'],c['threshold'],c['matches'],round(c['pooled_speedup'],3),[round(x,3) for x in c['process_speedups']])
