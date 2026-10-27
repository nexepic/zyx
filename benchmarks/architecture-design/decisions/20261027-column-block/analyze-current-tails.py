"""Review existing targeted samples without another timing campaign."""
from pathlib import Path
import json, statistics, hashlib
N=Path(__file__).resolve().parent
records=[]
for directory in ['current-boundary','current-cpu']:
    for case in ['all_nodes_property_filter','multi_key_group']:
        source=N/directory/(case+'.jsonl')
        rows=[json.loads(line) for line in source.read_text().splitlines() if line.startswith('{')]
        median=statistics.median(x['latency_ms'] for x in rows)
        tail=[]
        for x in rows:
            if x['latency_ms'] < 2*median:
                continue
            cpu=x['user_ms']+x['system_ms']
            tail.append({k:x[k] for k in ['iteration','latency_ms','sampled','user_ms','system_ms','major_faults','involuntary_switches']} | {'cpu_wall_ratio':cpu/x['latency_ms']})
        records.append({'source':str(source.relative_to(N)),'sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'samples':len(rows),'p50_ms':median,'max_ms':max(x['latency_ms'] for x in rows),'tails_over_2x_median':tail})
result={'selection_rule':'Describe all existing samples over 2x their own run median; this is retrospective diagnosis, not an acceptance filter. No samples removed.', 'runs':records,'findings':['Large tails recur in both targeted query classes.','The recorded tail CPU time is close to wall time, with zero major faults. Waiting off CPU or major page faults alone cannot explain these samples.','There are no per-tail retired-instruction/cycle counters or deterministic-work counts. Extra engine work versus changes in execution speed remain unresolved.','The original formal window lacks per-query CPU/fault/switch fields. Do not retroactively assign the same cause to its tails.','The multi-key CPU observer fails its perturbation gate; its sampled shares cannot support attribution. Its unobserved controls and raw timings are retained for diagnosis.']}
(N/'tail-review.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps([{'source':x['source'],'tails':len(x['tails_over_2x_median'])} for x in records]))
