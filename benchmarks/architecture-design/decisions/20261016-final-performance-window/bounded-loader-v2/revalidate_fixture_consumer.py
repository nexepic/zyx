from pathlib import Path
import argparse,importlib.util,json,subprocess,csv,time,shutil
s=importlib.util.spec_from_file_location('w','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(s);s.loader.exec_module(w)
p=argparse.ArgumentParser();p.add_argument('--scale',choices=['medium','holdout'],required=True);a=p.parse_args();w.restrict()
f=w.PREPARED/a.scale;source=f/'current.zyx';manifest=Path(str(source)+'.json');payload=json.loads(manifest.read_text());assert payload['database_sha256']==w.digest(source)
binary=w.CURRENT/'zyx-compare-bench';new_sha=w.digest(binary)
if payload['consumer_binary_sha256']==new_sha:print('Consumer already matches; no repeat observation');raise SystemExit(0)
original=dict(payload);(f/'current-manifest-before-consumer-revalidation.json').write_text(json.dumps(original,indent=2)+'\n')
o=f/'final-consumer-revalidation';o.mkdir();data=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/dataset') if a.scale=='medium' else w.PREPARED/'datasets/holdout'
with (data/'users.csv').open() as stream:expected=sum(v['country']=='CN' for v in csv.DictReader(stream))
command=[str(binary),'--dataset',str(data),'--prepared-db',str(source),'--db-path',str(o/'private.zyx'),'--scale',a.scale,'--profile','scan','--workload','label_scan_filter','--warmup','0','--iterations','1','--threads','1','--load-batch-rows','1000','--emit-result-rows']
with (o/'excluded-stdout.jsonl').open('w') as out,(o/'stderr.txt').open('w') as err:result=subprocess.run(command,stdout=out,stderr=err,timeout=1800)
assert result.returncode==0,(o/'stderr.txt').read_text()[-1500:]
values=[json.loads(v) for v in (o/'excluded-stdout.jsonl').read_text().splitlines() if v.startswith('{')];observed=[v for v in values if v.get('event')=='result'];assert len(observed)==1 and observed[0]['value']==expected
assert payload['database_sha256']==w.digest(source)
proof={'purpose':'Final consumer correctness only; zero final timing samples accepted; source bytes unchanged. Preparation producer and all version/batch lineage remain separate.','previous_consumer_sha256':payload['consumer_binary_sha256'],'consumer_binary_sha256':new_sha,'consumer_core_archive_sha256':w.digest(w.CURRENT/'libzyx_core.a'),'source_database_sha256':payload['database_sha256'],'command':command,'expected_label_count':expected,'observed_label_count':observed[0]['value'],'formal_timing_samples_accepted':0}
(o/'proof.json').write_text(json.dumps(proof,indent=2)+'\n');payload['consumer_binary_sha256']=new_sha;payload['final_consumer_revalidation']=proof;manifest.write_text(json.dumps(payload,indent=2)+'\n')
for path in o.glob('private.zyx*'):
 if path.is_file():path.unlink()
print(json.dumps({'consumer_revalidated':a.scale,'expected':expected}),flush=True)
