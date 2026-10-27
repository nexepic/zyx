"""Preserve original and targeted review windows; never select best rounds."""
from pathlib import Path
import json,statistics
D=Path(__file__).resolve().parent;B=D/'b-field-cohort';O=B/'guard-review-1'
assert (O/'summary.json').exists(), 'The preregistered reproduction must finish first'
original=json.loads((B/'final-guards/summary.json').read_text())
review=json.loads((O/'summary.json').read_text())
def stat(values):
    values=sorted(values);position=.95*(len(values)-1);index=int(position)
    return dict(samples=len(values),p50_ms=statistics.median(values),p95_ms=values[index]+(values[min(index+1,len(values)-1)]-values[index])*(position-index))
def samples(folder,scale,case,engine):
    values=[];process=[]
    for p in sorted(folder.glob(f'*-{scale}-*-{engine}.jsonl')):
        selected=[x['latency_ms'] for line in p.read_text().splitlines() if line.startswith('{') for x in [json.loads(line)] if x.get('case')==case and 'rows' in x]
        if selected:values.extend(selected);process.append({'file':str(p.relative_to(B)),'p50_ms':statistics.median(selected),'samples':len(selected)})
    return values,process
rows=[]
for old in original:
    if not old['review_required']:continue
    scale,case=old['scale'],old['case']
    current=next(x for x in review if x['scale']==scale and x['case']==case)
    record={'scale':scale,'case':case,'original':old,'review':current,'combined':{}}
    for engine in ('control','candidate'):
        before,p1=samples(B/'final-guards',scale,case,engine);after,p2=samples(O,scale,case,engine)
        assert len(before)==30 and len(after)==20,(scale,case,engine,len(before),len(after))
        record['combined'][engine]=stat(before+after)
        record['combined'][engine]['processes']=p1+p2
    for quantile in ('p50_ms','p95_ms'):
        record['combined']['candidate_over_control_'+quantile]=record['combined']['candidate'][quantile]/record['combined']['control'][quantile]
    record['review_still_flagged']=current['review_required']
    rows.append(record)
assert len(rows)==5
opened=[]
for folder in (B/'final-guards',O):
    for x in json.loads((folder/'open-close.json').read_text())['samples']:opened.append(dict(x,window=str(folder.relative_to(B))))
report={'scope':'All five originally flagged comparisons; original30 and targeted20 samples separately and pooled50, all five independent process medians retained. Pooling does not erase original failures or imply paired-stationary conditions. Read context cases are not additional primary acceptance samples.','rows':rows,'open_close_observations':opened,'open_caution':'Five observations per engine/dataset are descriptive only; high dispersion precludes startup latency promises.'}
(O/'combined-review.json').write_text(json.dumps(report,indent=2)+'\n')
for x in rows:print(x['scale'],x['case'],'review P50/P95',x['review']['candidate_over_control_p50'],x['review']['candidate_over_control_p95'],'combined',x['combined']['candidate_over_control_p50_ms'],x['combined']['candidate_over_control_p95_ms'])
