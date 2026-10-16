from pathlib import Path
import argparse,importlib.util,json,hashlib,subprocess,csv,shutil,os,time
spec=importlib.util.spec_from_file_location('window','/tmp/zyx-final-window.py');w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
p=argparse.ArgumentParser();p.add_argument('--engine',choices=['control','current'],required=True);p.add_argument('--scale',choices=['medium','holdout'],required=True);p.add_argument('--reuse-attempt',type=int);a=p.parse_args();eligible=set(os.sched_getaffinity(0));w.restrict()
if 'ZYX_PREPARATION_CPU' in os.environ:
 cpu=int(os.environ['ZYX_PREPARATION_CPU']);assert cpu in eligible;os.sched_setaffinity(0,{cpu})
root=Path('/workspace/zyx-optimization-final-window-20261016/prepared-inputs');folder=root/a.scale;folder.mkdir(parents=True,exist_ok=True)
binary=w.CONTROL/'zyx-compare-bench-bounded' if a.engine=='control' else w.CURRENT/'zyx-compare-bench'
core=w.CONTROL/'libzyx_core.a' if a.engine=='control' else w.CURRENT/'libzyx_core.a'
data=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation/dataset') if a.scale=='medium' else root/'datasets'/'holdout'
if not data.exists():
 from dataset.generate import SCALES,generate_graph,write_dataset
 graph=generate_graph(SCALES[a.scale],seed=314159);write_dataset(graph,data);del graph
source=folder/f'{a.engine}.zyx';manifest=Path(str(source)+'.json')
if source.exists() or manifest.exists():raise ValueError('never overwrite a prepared fixture')
with (data/'users.csv').open() as stream:expected=sum(row['country']=='CN' for row in csv.DictReader(stream))
start=time.monotonic();producer=binary
if a.reuse_attempt:
 if a.engine!='current' or a.scale!='medium':raise ValueError('reuse is limited to the current medium correctness preparation')
 previous=Path('/workspace/zyx-optimization-final-window-20261016/bounded-preparation')
 proof=json.loads((previous/f'attempt-{a.reuse_attempt}-passed.json').read_text());assert proof['passed'] and proof['label_count']==expected
 recorded=previous/f'attempt{a.reuse_attempt}-binary-provenance.json'
 if recorded.exists():
  provenance=json.loads(recorded.read_text());assert provenance['core_archive_sha256']==w.digest(core)
  producer=previous/f'attempt{a.reuse_attempt}-binary'
 else:
  producer=previous/f'attempt{a.reuse_attempt}-binary'
  assert producer.exists() and w.digest(producer)==w.digest(binary)
 shutil.copyfile(proof['database'],source)
else:
 command=[str(binary),'--dataset',str(data),'--db-path',str(source),'--scale',a.scale,'--profile','scan','--workload','label_scan_filter','--warmup','0','--iterations','1','--threads','1','--load-batch-rows','1000','--emit-result-rows']
 (folder/f'{a.engine}-preparation-command.json').write_text(json.dumps(command,indent=2)+'\n')
 result=subprocess.run(command,capture_output=True,text=True,timeout=7200)
 (folder/f'{a.engine}-excluded-preparation-stdout.jsonl').write_text(result.stdout);(folder/f'{a.engine}-preparation-stderr.txt').write_text(result.stderr)
 if result.returncode:raise RuntimeError(f'preparation failed: {source}; {result.stderr[-1000:]}')
 values=[json.loads(v) for v in result.stdout.splitlines() if v.startswith('{')];observed=[v for v in values if v.get('event')=='result'];assert len(observed)==1 and observed[0]['value']==expected,observed
payload={'status':'frozen_prepared_read_input','scale':a.scale,'seed':42 if a.scale=='medium' else 314159,'engine':a.engine,'consumer_binary_sha256':w.digest(binary),'producer_binary_sha256':w.digest(producer),'production_core_archive_sha256':w.digest(core),'database_sha256':w.digest(source),'dataset_sha256':{v.name:w.digest(v) for v in sorted(data.glob('*.csv'))},'source_profile':'scan with the baseline User.id property index','expected_label_count':expected,'preparation_transaction_rows':1000,'formal_timing_samples_accepted':0,'untimed_preparation_timeout_seconds':7200,'reuse_attempt':a.reuse_attempt,'preparation_affinity':sorted(os.sched_getaffinity(0)),'elapsed_preparation_seconds':time.monotonic()-start,'query_setup':'Each independent process clones this immutable file and creates only missing indexes for its registered read profile; input maps used only by import/write workloads are absent in prepared reads.'}
manifest.write_text(json.dumps(payload,indent=2)+'\n');source.chmod(0o444);print(json.dumps({'prepared_fixture':str(source),'database_sha256':payload['database_sha256'],'expected_label_count':expected,'formal_timing_samples_accepted':0}),flush=True)
