from pathlib import Path
import json,os,subprocess,time,statistics,hashlib,csv,sys
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design/results/20261022-core-cost';dest=root/'logical-radix-targeted';sys.path.insert(0,str(repo/'benchmarks/architecture-design/decisions/20261022-core-cost-final'));from oracle import check_event
source=root/'final/inputs/medium-summary.zyx';env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu');results=[];sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
binaries={'before':root/'lazy-capacity-targeted/public-baseline','after':dest/'public-baseline'}
freeze={'purpose':'targeted before/after validation of measured logical radix COW preservation cost, not final full-matrix acceptance','progress_percent':90,'source':str(source),'source_sha256':sha(source),'binaries':{k:{'path':str(p),'sha256':sha(p)} for k,p in binaries.items()},'after_library_sha256':sha(Path('/workspace/zyx-build-core-candidate/libzyx_core.a')),'before_library_sha256':sha(Path('/workspace/zyx-lazy-capacity-candidate/libzyx_core.a')),'warmup':3,'iterations':10,'rounds':3,'scales':['medium'],'cases':['property_set','edge_batch_1000']}
(dest/'targeted-freeze.json').write_text(json.dumps(freeze,indent=2)+'\n')
for round_number,order in enumerate([['before','after'],['after','before'],['before','after']],1):
 for case in freeze['cases']:
  for version in order:
   stem=dest/f'{round_number}-{case}-{version}';target=stem.with_suffix('.db');assert not target.exists() and not stem.with_suffix('.json').exists()
   cmd=[str(binaries[version]),'write',str(source),str(target),case,'3','10'];start=time.monotonic();print('START',round_number,case,version,flush=True)
   with stem.with_suffix('.jsonl').open('w') as out,stem.with_suffix('.stderr').open('w') as err:
    p=subprocess.Popen(cmd,env=env,stdout=out,stderr=err);pid,status,usage=os.wait4(p.pid,0);p.returncode=os.waitstatus_to_exitcode(status)
   events=[json.loads(l) for l in stem.with_suffix('.jsonl').read_text().splitlines() if l.startswith('{')]
   assert p.returncode==0 and len(events)==10,(cmd,p.returncode,events)
   for e in events:check_event(case,e,'medium',True)
   record={'round':round_number,'case':case,'version':version,'status':'passed','samples':events,'command':cmd,'process_wall_seconds':time.monotonic()-start,'process_user_seconds':usage.ru_utime,'process_system_seconds':usage.ru_stime,'process_peak_rss_bytes':usage.ru_maxrss*1024,'process_os_read_bytes':usage.ru_inblock*512,'process_os_write_bytes':usage.ru_oublock*512,'resources_include_private_clones_and_open_close':True}
   stem.with_suffix('.json').write_text(json.dumps(record,indent=2)+'\n');results.append(record);print('FINISH',round_number,case,version,round(statistics.median(e['latency_ms'] for e in events),4),flush=True)
with (dest/'targeted-summary.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['case','version','samples','p50_ms','p95_ms'])
 for case in freeze['cases']:
  for version in ['before','after']:
   values=[e['latency_ms'] for r in results if r['case']==case and r['version']==version for e in r['samples']];w.writerow([case,version,len(values),statistics.median(values),sorted(values)[28]])
(dest/'targeted-completion.json').write_text(json.dumps({'status':'passed','processes':len(results),'progress_percent':90,'full_matrix':False,'kuzu_acceptance':False},indent=2)+'\n')
