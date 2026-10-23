"""Untimed semantics and preparation checks before the registered baseline."""
from pathlib import Path
import json, os, subprocess, sys
from oracle import check_event
REPO=Path(__file__).resolve().parents[4]
HERE=Path(__file__).resolve().parent
OLD=REPO/'benchmarks/architecture-design/results/20261020-work-resume'
RUN=REPO/'benchmarks/architecture-design/results/20261022-core-cost/final'
ENV=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu')
PYTHON='/workspace/zyx-tools/bin/python'
def run():
 results=[]
 for scale in ['medium','holdout']:
  for mode,cases,source in [('read','label_scan_filter,aggregation_count_by_group,multi_key_group','r43-identity'),('write','property_set','r7-final-indexed'),('write','edge_batch_1000','r7-final-indexed')]:
   for engine in ['zyx','kuzu']:
    target=RUN/f'preflight-{scale}-{engine}-{cases.split(",")[0]}.db'
    inp=OLD/f'r7-inputs/{scale}-{source}.zyx' if engine=='zyx' else OLD/f'r7-inputs/{scale}-r7-final.kuzu'
    if engine=='zyx' and mode=='write':inp=RUN/'inputs'/f'{scale}-summary.zyx'
    cmd=([str(RUN/'public-baseline')] if engine=='zyx' else [PYTHON,str(HERE/'kuzu-worker.py')])+['check-'+mode,str(inp),str(target),cases,'0','1']
    p=subprocess.run(cmd,env=ENV,text=True,capture_output=True,timeout=90)
    stem=RUN/target.stem
    stem.with_suffix('.jsonl').write_text(p.stdout);stem.with_suffix('.stderr').write_text(p.stderr)
    if p.returncode:raise RuntimeError(p.stderr)
    events=[json.loads(l) for l in p.stdout.splitlines() if l.startswith('{')]
    assert len(events)==len(cases.split(',')),(cmd,events)
    for e in events:check_event(e['case'],e,scale,mode=='write')
    for private in target.parent.glob(target.name+'*'):
     if private.is_file():private.unlink()
    results.append({'scale':scale,'engine':engine,'cases':cases,'passed':True,'command':cmd})
    print(scale,engine,cases,'passed',flush=True)
 (HERE/'preflight.json').write_text(json.dumps(results,indent=2)+'\n')
if __name__=='__main__':run()
