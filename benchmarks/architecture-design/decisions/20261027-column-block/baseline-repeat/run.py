"""User-requested independent primary comparison; preserve earlier windows."""
from pathlib import Path
import hashlib,json,math,statistics,subprocess,sys,datetime
R=Path('/workspace/zyx');D=R/'benchmarks/architecture-design/decisions/20261026-scan-supply-v2';B=D/'b-field-cohort';O=Path(__file__).resolve().parent
sys.path.insert(0,str(D));from process_receipt import run
sys.path.insert(0,str(R/'benchmarks/architecture-design/decisions/20261025-final-scan-comparison'));import campaign
sha=lambda p:hashlib.file_digest(Path(p).open('rb'),'sha256').hexdigest()
original=json.loads((B/'final-primary/freeze.json').read_text())
binaries={k:Path(v['path']) for k,v in original['zyx_binaries'].items()};kuzu=Path(original['kuzu_worker']['path'])
def verify():
 for k,p in binaries.items():assert sha(p)==original['zyx_binaries'][k]['sha256'],('binary',k)
 assert sha(kuzu)==original['kuzu_worker']['sha256']
 for p,h in original['source_hashes'].items():assert sha(R/p)==h,('source',p)
 for scale in ('medium','holdout'):
  for p in (B/(scale+'.zyx'),campaign.INPUT_ROOT/(scale+'.scan-corrected-v1.zyx'),campaign.INPUT_ROOT/(scale+'.kuzu')):assert sha(p)==original[str(p)],('input',p)
assert not (O/'protocol.json').exists(),'Never overwrite a measurement window'
print('VERIFY frozen engines, committed engine sources and six input hashes',flush=True);verify()
assert sha(binaries['control'])==json.loads((D.parent/'20261026-scan-supply-v0/baseline/freeze.json').read_text())['zyx_binary']['sha256']
assert json.loads((D/'b-cohort-strengthened/receipt.json').read_text())['returncode']==0
cases=original['cases'];orders=[['control','candidate','kuzu'],['candidate','kuzu','control'],['kuzu','control','candidate']]
protocol={'request':'Fresh before/after and Kuzu comparison; previous windows unchanged','head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'start_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'prior_freeze_sha256':sha(B/'final-primary/freeze.json'),'runner_sha256':sha(__file__),'zyx_binaries':original['zyx_binaries'],'source_hashes':original['source_hashes'],'kuzu_worker':original['kuzu_worker'],'kuzu_version':subprocess.check_output([campaign.PYTHON,'-c','import kuzu;print(kuzu.__version__)'],env=campaign.ENV,text=True).strip(),'cases':cases,'scales':['medium','holdout'],'orders':orders,'warmups':3,'iterations':10,'processes_per_engine_dataset':3,'cpu_affinity':[0],'threads':1,'buffer_pool_bytes':67108864,'query_materialized_budget_bytes':67108864,'limits':campaign.REG['limits'],'scope':'Warm opened-handle query execution and complete result consumption; excludes copying, opening and offline conversion. Candidate is experimental SCAN cohort8, not default ROW/profile performance.','baseline':'Exact V0 frozen pre-supply-optimization binary and image; candidate uses committed engine and immutable B image. Not the earliest project version.'}
(O/'protocol.json').write_text(json.dumps(protocol,indent=2)+'\n');rows=[]
for rnd,order in enumerate(orders):
 for scale in ('medium','holdout'):
  for engine in order:
   stem=O/f'{rnd}-{scale}-{engine}';target=Path('/tmp')/f'zyx-user-20261027-primary-{rnd}-{scale}-{engine}.db';assert not target.exists()
   source=B/(scale+'.zyx') if engine=='candidate' else campaign.INPUT_ROOT/(scale+('.kuzu' if engine=='kuzu' else '.scan-corrected-v1.zyx'))
   cmd=['/usr/bin/taskset','-c','0']+([campaign.PYTHON,str(kuzu)] if engine=='kuzu' else [str(binaries[engine])])+['read',str(source),str(target),','.join(cases),'3','10']
   print('START',rnd,scale,engine,flush=True)
   with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:resources=run(cmd,out,err,campaign.ENV,campaign.REG['limits'])
   receipt={'command':cmd,'resources':resources,'oracle':'pending'};stem.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
   assert resources['returncode']==0 and not resources['timed_out'],receipt
   events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')];assert len(events)==40,(stem,len(events))
   for e in events:
    campaign.check_event(e['case'],e,scale,False);assert math.isfinite(e['latency_ms']) and e['latency_ms']>=0
    rows.append(dict(e,engine=engine,scale=scale,round=rnd))
   receipt.update(oracle='passed',samples=len(events));stem.with_suffix('.receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');print('PASS',rnd,scale,engine,flush=True)
def stat(xs):
 xs=sorted(xs);q=.95*(len(xs)-1);i=int(q)
 return dict(p50_ms=statistics.median(xs),p95_ms=xs[i]+(xs[min(i+1,len(xs)-1)]-xs[i])*(q-i),samples=len(xs))
summary=[]
for scale in ('medium','holdout'):
 for case in cases:
  record=dict(scale=scale,case=case)
  for engine in ('control','candidate','kuzu'):
   selected=[x for x in rows if x['scale']==scale and x['case']==case and x['engine']==engine];assert len(selected)==30
   record[engine]=stat([x['latency_ms'] for x in selected]);record[engine]['process_p50_ms']=[statistics.median(x['latency_ms'] for x in selected if x['round']==rnd) for rnd in range(3)]
  record['control_over_candidate']=record['control']['p50_ms']/record['candidate']['p50_ms'];record['candidate_over_kuzu']=record['candidate']['p50_ms']/record['kuzu']['p50_ms'];summary.append(record)
print('VERIFY post-window hashes',flush=True);verify();assert len(rows)==720
(O/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
gain=math.exp(statistics.mean(math.log(x['control_over_candidate']) for x in summary));gap=[x['candidate_over_kuzu'] for x in summary]
result={'oracles':720,'all_passed':True,'hashes_verified_before_after':True,'primary_geomean_speedup':gain,'candidate_over_kuzu_range':[min(gap),max(gap)],'allnode_each':[x['control_over_candidate'] for x in summary if x['case']==cases[0]],'kuzu_target_passed':all(x<=1 for x in gap),'finish_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()};(O/'result.json').write_text(json.dumps(result,indent=2)+'\n')
header='''# 性能对比：本轮优化前、优化后与 Kuzu\n\n本次为用户要求的新测量窗口；旧窗口保持不变。优化前使用 V0 冻结版本，优化后对应当前提交的实验性 SCAN/cohort8，不代表默认 ROW 模式。Kuzu 版本0.11.3。\n\n固定单线程、CPU0、64MiB缓存；medium/holdout、四项主查询，每引擎三独立进程，每进程三次预热、十次正式样本。P50/P95均为ms，包含完整查询结果消费，不包含复制、打开认证和离线转换。全部720个完整结果oracle通过，前后源码/二进制/输入哈希一致。\n\n| 数据集 / 查询 | 优化前 P50 / P95 | 优化后 P50 / P95 | Kuzu P50 / P95 | 前/后 P50 | 后/Kuzu P50 |\n| --- | ---: | ---: | ---: | ---: | ---: |\n'''
for x in summary:
 vals=[f"{x[e]['p50_ms']:.3f} / {x[e]['p95_ms']:.3f}" for e in ('control','candidate','kuzu')];header+=f"| {x['scale']} / {x['case']} | {' | '.join(vals)} | {x['control_over_candidate']:.3f}x | {x['candidate_over_kuzu']:.2f}x |\n"
header+=f'\n八项P50收益几何平均为 **{gain:.4f}倍**。优化后相对Kuzu为 **{min(gap):.2f}–{max(gap):.2f}倍耗时**；超过Kuzu目标：{result["kuzu_target_passed"]}。每项三进程中位数保留于summary.json，不选择最好进程。\n\n本次只重测主查询，未重新测写入、转换和打开成本。上一轮确认的写入回退与采用限制继续保留，见../../decisions/20261026-scan-supply-v2/README.zh.md。该重复窗口用于观察可重复性，不替换旧验收记录或增加优化进度。\n'
(O/'README.md').write_text(header);print('COMPLETE',json.dumps(result),flush=True)
