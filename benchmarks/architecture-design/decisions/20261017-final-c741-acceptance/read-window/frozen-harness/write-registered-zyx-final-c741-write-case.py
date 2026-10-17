from pathlib import Path
import argparse,csv,hashlib,importlib.util,json,os,resource,shutil,sys,tempfile,time
sys.path.insert(0,'/workspace/zyx/benchmarks/graphdb-comparison')
spec=importlib.util.spec_from_file_location('window','/tmp/zyx-final-c741-window.py');w=importlib.util.module_from_spec(spec);spec.loader.exec_module(w)
from runner.workload_preflight import NativeWorker,compare_rows
p=argparse.ArgumentParser();p.add_argument('--engine',required=True,choices=['control','current','kuzu']);p.add_argument('--round',type=int,required=True);p.add_argument('--samples',type=int,default=10);p.add_argument('--workload',action='append');a=p.parse_args();w.restrict()
import importlib
owned_case=importlib.import_module('zyx-final-five-owned-case')
owned_case.install_temp_journal(w.OUT,f'write-round-{a.round}-{a.engine}',tempfile)
root=w.OUT/'write-protection'/f'round-{a.round}'/a.engine;root.mkdir(parents=True,exist_ok=False)
body='w'*(1024*1024);records=[];checks=[];restores=[];native=None;db=None;conn=None
queries={
'set_score_large_body':'MATCH (n:User {id:0}) SET n.score=777 RETURN 1 AS changed',
'unchanged_score_large_body':'MATCH (n:User {id:0}) SET n.score=0 RETURN 1 AS changed',
'remove_age':'MATCH (n:User {id:0}) REMOVE n.age RETURN 1 AS changed',
'create_nodes_100':'CREATE '+','.join(f'(:Written {{id:{i},value:1}})' for i in range(100))+' RETURN 1 AS changed',
'create_nodes_1000':'CREATE '+','.join(f'(:Written {{id:{i},value:1}})' for i in range(1000))+' RETURN 1 AS changed',
'create_edge':'MATCH (a:User {id:0}),(b:User {id:1}) CREATE (a)-[:FOLLOWS {weight:2}]->(b) RETURN 1 AS changed',
'delete_edge':'MATCH ()-[r:FOLLOWS]->() DELETE r RETURN 1 AS changed',
'delete_node':'MATCH (n:User {id:9999}) DELETE n RETURN 1 AS changed'}
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()
def file_manifest(folder):return {str(p.relative_to(folder)):digest(p) for p in folder.rglob('*') if p.is_file()}
def run(query,expected):
 if native is not None:return native.call('QUERY',query,expected=expected,ordered=True)
 result=conn.execute(query)
 try:
  rows=[]
  while result.has_next():rows.append(result.get_next())
  if rows!=expected:raise AssertionError(f'post-state/output mismatch: {len(rows)} vs{len(expected)} rows for {query[:120]}')
 finally:result.close()
 return rows
try:
 with tempfile.TemporaryDirectory(prefix='zyx-write-protection-') as directory:
  folder=Path(directory);private=folder/'private';template=folder/'template';private.mkdir();template.mkdir();path=private/'database.zyx'
  if a.engine!='kuzu':
   binary=Path('/workspace/zyx-optimization-control-20261015/binaries/zyx-workload-contract-worker') if a.engine=='control' else Path(json.loads((w.OUT/'registration.json').read_text())['current_frozen_worker'])
   native=NativeWorker(binary,path,root);native.call('FORMAT')
  else:
   import kuzu
   w.kuzu_limits();db=kuzu.Database(str(path),buffer_pool_size=64*1024**2,max_num_threads=1);conn=kuzu.Connection(db)
   for query in ['CREATE NODE TABLE User(id INT64, country STRING, score INT64, age INT64, body STRING, PRIMARY KEY(id))','CREATE NODE TABLE Written(id INT64, value INT64, PRIMARY KEY(id))','CREATE REL TABLE FOLLOWS(FROM User TO User, weight INT64)']:
    result=conn.execute(query);result.close()
  for start in range(0,10000,100):
   query='CREATE '+','.join(f"(:User {{id:{i},country:'country-{i%8}',score:{i%7},age:{18+i%53}}})" for i in range(start,start+100))+' RETURN 1 AS changed';run(query,[[1]])
  if native is not None:native.call('QUERY','CREATE INDEX write_protection_id FOR (n:User) ON (n.id)')
  if native is not None:native.call('PARAM','MATCH (n:User {id:0}) SET n.body=$body RETURN 1 AS changed','body','string',body,expected=[[1]])
  else:
   result=conn.execute('MATCH (n:User {id:0}) SET n.body=$body RETURN 1 AS changed',{'body':body})
   assert result.has_next() and result.get_next()==[1];result.close()
  run('CREATE (:Written {id:-1,value:0}) RETURN 1 AS changed',[[1]])
  run('MATCH (n:Written) DELETE n RETURN 1 AS changed',[[1]])
  if native is not None:native.call('QUERY','CREATE INDEX write_protection_written_id FOR (n:Written) ON (n.id)')
  run('MATCH (a:User {id:0}),(b:User {id:1}) CREATE (a)-[:FOLLOWS {weight:1}]->(b) RETURN 1 AS changed',[[1]])
  if native is not None:
   native.call('READONLY');shutil.copyfile(path,template/'database.zyx')
  else:
   conn=None;db.close();db=None;shutil.copytree(private,template,dirs_exist_ok=True)
  baseline=file_manifest(template);(root/'baseline-manifest.json').write_text(json.dumps({'files':baseline,'logical_fixture':'10000 User rows, one1MiB body onUser0, empty Written namespace, oneFOLLOWS edge0->1(weight1)','producer':'d09 frozen control' if a.engine=='control' else ('c741 frozen candidate' if a.engine=='current' else 'Kuzu0.11.3'),'input_seed':42,'cloning_excluded_from_timer':True},indent=2)+'\n')
  for workload,query in queries.items():
   if a.workload and workload not in a.workload:continue
   if a.engine=='kuzu' and workload=='remove_age':
    checks.append({'workload':workload,'status':'unsupported_NA','reason':'Native REMOVE makes an actual missing key; fixed Kuzu schema retains a null column. Do not equate these semantics.'});continue
   for sample in range(3+a.samples):
    if native is not None:
     replacement=private/'database.next';shutil.copyfile(template/'database.zyx',replacement);assert digest(replacement)==baseline['database.zyx'];os.replace(replacement,path);native.call('REOPEN')
    else:
     if db is not None:conn=None;db.close();db=None
     shutil.rmtree(private);shutil.copytree(template,private);assert file_manifest(private)==baseline
     db=kuzu.Database(str(path),buffer_pool_size=64*1024**2,max_num_threads=1);conn=kuzu.Connection(db)
    run('MATCH (n:User {id:0}) RETURN n.id AS id,n.score AS score',[[0,0]])
    # Reopen again so baseline validation does not warm different field pages.
    if native is not None:native.call('REOPEN');before=native.call('STATS')
    else:conn=None;db.close();db=kuzu.Database(str(path),buffer_pool_size=64*1024**2,max_num_threads=1);conn=kuzu.Connection(db);before=None
    begin=time.perf_counter_ns();run(query,[[1]]);elapsed=time.perf_counter_ns()-begin
    after=native.call('STATS') if native is not None else None
    if native is not None:native.call('READONLY')
    else:conn=None;db.close();db=kuzu.Database(str(path),buffer_pool_size=64*1024**2,max_num_threads=1);conn=kuzu.Connection(db)
    if workload in ['set_score_large_body','unchanged_score_large_body']:
     run('MATCH (n:User {id:0}) RETURN n.id AS id,n.score AS score,n.body AS body',[[0,777 if workload=='set_score_large_body' else 0,body]])
    elif workload=='remove_age':run('MATCH (n:User {id:0}) RETURN properties(n) AS fields',[[{'id':0,'country':'country-0','score':0,'body':body}]])
    elif workload.startswith('create_nodes_'):
     count=int(workload.rsplit('_',1)[1]);run('MATCH (n:Written) RETURN n.id AS id,n.value AS value ORDER BY id',[[i,1] for i in range(count)])
    elif workload in ['create_edge','delete_edge']:
     run('MATCH (a:User {id:0})-[r:FOLLOWS]->(b:User {id:1}) RETURN r.weight AS weight ORDER BY weight',[[1],[2]] if workload=='create_edge' else [])
    else:
     run('MATCH (n:User {id:9999}) RETURN n.id AS id',[]);run('MATCH (n:User) RETURN count(n) AS count',[[9999]])
    if native is not None:native.call('INTEGRITY')
    item={'workload':workload,'phase':'warmup' if sample<3 else 'measure','iteration':sample-3 if sample>=3 else sample,'elapsed_ns':elapsed,'full_poststate_verified_after_reopen':True,'before':before,'after':after,'acknowledgement':'default statement/autocommit acknowledgement; no added checkpoint inside timer; durability implementations differ and no strict engine-only win is claimed'}
    records.append(item);(root/'samples.json').write_text(json.dumps(records,indent=2)+'\n')
   assert file_manifest(template)==baseline
   print(json.dumps({'workload':workload,'engine':a.engine,'round':a.round,'warmups':3,'measured':a.samples,'poststate':'passed'}),flush=True)
  if native is not None:native.close();native=None
  elif db is not None:conn=None;db.close();db=None
 (root/'completed.json').write_text(json.dumps({'engine':a.engine,'round':a.round,'status':'completed','samples':records,'unsupported':checks,'worker_processes':1 if a.engine!='kuzu' else 0,'schema_difference':'Native nonunique User.id/Written.id property indexes versus Kuzu primary keys; no strict schema-equivalent cross-engine win claim',
'sampling_unit':'one native worker or Kuzu in-process engine per independent round, each workload has3warmup+10measured restored-baseline samples; backend opens and validation outside timer','native_restore':'Private inode replaced only while backend is physically read-only; REOPEN creates fresh backend. Original immutable template is hash-verified; Linux-only benchmark orchestration, no production API change.','timer_scope':'Native IPC execute/consume/scalar-check/bookkeeping versus Kuzu Python execute/drain/check; entry overhead differs; post-state/reopen/cloning excluded','resource_scope':'whole profile including baseline creation and validation; native launches exactly one worker and no Git helper','rss_max_kib':resource.getrusage(resource.RUSAGE_CHILDREN if a.engine!='kuzu' else resource.RUSAGE_SELF).ru_maxrss,'driver_rss_max_kib':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss},indent=2)+'\n')
finally:
 if native is not None:native.close()
 if db is not None:conn=None;db.close()
