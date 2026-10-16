from pathlib import Path
from collections import defaultdict
import json,statistics,math
root=Path('/workspace/zyx-optimization-final-window-20261016/bounded-loader-v2')
def quantile(values,p):
 ordered=sorted(values);index=(len(ordered)-1)*p;lo=math.floor(index);hi=math.ceil(index)
 return ordered[lo]+(ordered[hi]-ordered[lo])*(index-lo)
reports=[]
for folder in sorted((root/'contract').glob('round-*/*')):
 if not (folder/'campaign-case-completed.json').exists():continue
 report=json.loads((folder/'report.json').read_text());timing=json.loads((folder/'ipc-operation-timings.json').read_text())
 engine=folder.name;operations=timing['operations'];writes=[];windows=[];active=None
 for operation in operations:
  command=operation['command'];query=operation.get('query','');ns=operation['ns']
  is_insert='(:Churn' in query and query.startswith('CREATE')
  if is_insert:
   writes.append(ns)
   if active is None:active={'foreground_ns':0,'maintenance_ns':0,'reopen_integrity_ns':0,'write_observations':0}
   active['write_observations']+=1
  if active is not None:
   if command=='MAINTAIN' or query=='CHECKPOINT':active['maintenance_ns']+=ns
   elif command in ['REOPEN','INTEGRITY','DB_OPEN','DB_CLOSE']:active['reopen_integrity_ns']+=ns
   elif command!='STATS':active['foreground_ns']+=ns
   if command=='INTEGRITY' or (engine=='kuzu' and command=='DB_OPEN'):
    if active['write_observations']:
     windows.append(active);active=None
 if active:windows.append(active)
 plateau=report['maintenance_windows'];assert len(plateau)==10
 files=[v['file_bytes'] for v in plateau]
 entry={'round':folder.parent.name,'engine':engine,'scope':timing['scope'],'tail_write_observations':len(writes),'tail_requirement_met':len(writes)>=1000,'insert_p50_ms':statistics.median(writes)/1e6 if writes else None,'insert_p95_ms':quantile(writes,.95)/1e6 if writes else None,'maintenance_windows':plateau,'accounted_window_timings':windows,'last_five_file_min_bytes':min(files[-5:]),'last_five_file_max_bytes':max(files[-5:]),'last_five_file_growth_fraction':files[-1]/files[-5]-1,'maxrss_kib':json.loads((folder/'elapsed.json').read_text())['maxrss_kib'],'backlog':'explicit reclaimer observations retained in native trace; no invented global backlog counter'}
 reports.append(entry)
(root/'steady-state-comparison.json').write_text(json.dumps({'status':'observations_only_requires_gate_review','tail_policy':'at least 1000 per-process durable insert observations; do not pool independent processes','comparability':'Native IPC includes serialization and semantic checks; Kuzu Python includes execute/drain/close; report entry overhead and schema/index differences, no unsupported strict engine-only ratio','cases':reports,'failed_frozen_control_cases':[json.loads(p.read_text()) for p in (root/'contract').glob('round-*/control/campaign-case-failed.json')]},indent=2)+'\n')
print(json.dumps({'cases':len(reports),'tail_complete_cases':sum(v['tail_requirement_met'] for v in reports)}))
