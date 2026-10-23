from pathlib import Path
import sys,json,time,os,re,shutil
import kuzu
r=Path(__file__).resolve().parent
mode,source,target,selected,warm,iterations=sys.argv[1:];source=Path(source);target=Path(target);warm=int(warm);iterations=int(iterations)
require=lambda x: (_ for _ in ()).throw(RuntimeError('invalid worker contract')) if not x else None
def sync_copy(path):
 with path.open("rb") as f:os.fsync(f.fileno())
require(source.is_file() and not target.exists());shutil.copyfile(source,target);sync_copy(target)
text=(r/'public-probe.cpp').read_text();queries={name:json.loads('"'+sql+'"') for name,sql in re.findall(r'if\(name=="([^"]+)"\)return "((?:[^"\\]|\\.)*)";',text)}
queries['distinct_tuple']='MATCH (u:User) WITH DISTINCT u.country AS country,u.age AS age RETURN count(*) AS count'
unsupported={'property_equality_indexed','property_range_indexed','property_index_fetch_users_by_country','range_index_fetch_user_projection'}
def typed(value):
 if value is None:return ['null',None]
 if isinstance(value,bool):return ['boolean',value]
 if isinstance(value,int):return ['integer',value]
 if isinstance(value,float):return ['double',value]
 if isinstance(value,str):return ['string',value]
 if isinstance(value,dict) and '_label' in value and '_id' in value:
  return ['node',{'internal_id':value['_id']['offset'],'table':value['_id']['table'],'identity_kind':'kuzu_offset','labels':[value['_label']],'properties':{k:typed(v) for k,v in value.items() if not k.startswith('_')}}]
 raise TypeError('unsupported observed Kuzu value '+str(type(value)))
def capture(connection,query):
 result=connection.execute(query)
 try:
  rows=[]
  while result.has_next():rows.append(list(result.get_next()))
  return rows
 finally:result.close()
def output(value):print(json.dumps(value,separators=(',',':')),flush=True)
def open_db():
 database=kuzu.Database(str(target),buffer_pool_size=64*1024**2,max_db_size=2*1024**3,max_num_threads=1)
 connection=kuzu.Connection(database,num_threads=1);return database,connection
def close_db(database,connection):connection.close();database.close()
def size():return sum(p.stat().st_size for p in target.parent.glob(target.name+'*') if p.is_file())
def execute(connection,name):
 if name=='edge_create_delete':
  capture(connection,"MATCH (a:User {id:'user-000001'}),(b:User {id:'user-000002'}) CREATE (a)-[:FOLLOWS {weight:-9999}]->(b)")
  return capture(connection,"MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000002'}) WHERE r.weight=-9999 DELETE r RETURN 1 AS value")
 if name.startswith('edge_batch_'):
  count=int(name.rsplit('_',1)[1]);return capture(connection,f"MATCH (a:User {{id:'user-000001'}}),(b:User {{id:'user-000002'}}) UNWIND range(1,{count}) AS ordinal CREATE (a)-[:FOLLOWS {{weight:1}}]->(b) RETURN count(*) AS count")
 return capture(connection,queries[name])
def postquery(name):
 if name in ['edge_create','edge_create_delete']:return "MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000002'}) WHERE r.weight=-9999 RETURN count(r)"
 if name.startswith('edge_batch_'):return "MATCH (:User {id:'user-000001'})-[r:FOLLOWS]->(:User {id:'user-000002'}) WHERE r.weight=1 RETURN count(r)"
 if name in ['property_set','property_noop']:return "MATCH (u:User {id:'user-000006'}) RETURN u.score AS value"
 return None
writing=mode in ['write','check-write'];timing=mode in ['read','read-opened','write'];cases=selected.split(',')
if cases==['steady_property']:
 database,writer=open_db();reader=kuzu.Connection(database,num_threads=1)
 capture(reader,'BEGIN TRANSACTION READ ONLY');old=capture(reader,"MATCH (u:User {id:'user-000006'}) RETURN u.score")[0][0]
 initial=size();peak=initial;at_drain=initial;count=40 if timing else 4;release=count//2
 for index in range(count):
  expected=-1000.0-index;query=f"MATCH (u:User {{id:'user-000006'}}) SET u.score={expected:.6f} RETURN u.score AS value"
  start=time.perf_counter() if timing else None;rows=capture(writer,query);elapsed=(time.perf_counter()-start)*1000 if timing else None
  require(rows==[[expected]])
  if index<release:require(capture(reader,"MATCH (u:User {id:'user-000006'}) RETURN u.score")==[[old]])
  if index+1==release:capture(reader,'ROLLBACK');reader.close();at_drain=size()
  peak=max(peak,size())
  if index>=3 or not timing:
   event={'timing_benchmark':timing,'case':'steady_property','iteration':index,'old_pin_held':index<release,'file_bytes':size(),'rows':[[typed(x) for x in row] for row in rows]}
   if timing:event['latency_ms']=elapsed
   output(event)
 close_db(database,writer);output({'event':'steady_space','initial':initial,'at_pin_drain':at_drain,'peak':peak,'final':size()})
else:
 database=connection=None
 if not writing and mode!='read-opened':database,connection=open_db()
 for name in cases:
  if name in unsupported:output({'case':name,'status':'unsupported_NA','reason':'Kuzu0.11.3has no scalar secondary property index capability; no scan relabeled as indexed'});continue
  try:
   for index in range(warm+iterations):
    if writing and index:
     for p in target.parent.glob(target.name+'*'):
      if p.is_file():p.unlink()
     shutil.copyfile(source,target);sync_copy(target)
    opened=time.perf_counter() if timing else None
    if writing or mode=='read-opened':database,connection=open_db()
    open_ms=(time.perf_counter()-opened)*1000 if timing else None
    start=time.perf_counter() if timing else None;rows=execute(connection,name);elapsed=(time.perf_counter()-start)*1000 if timing else None
    verify=postquery(name) if writing else None;persisted=capture(connection,verify) if verify else None
    if index>=warm:
     event={'timing_benchmark':timing,'case':name,'iteration':index-warm,'rows':[[typed(x) for x in row] for row in rows]}
     if timing:event['latency_ms']=elapsed
     if persisted is not None:event['postcondition_rows']=[[typed(x) for x in row] for row in persisted]
     output(event)
    if writing or mode=='read-opened':
     closing=time.perf_counter() if timing else None;close_db(database,connection);connection=database=None
     if mode=='read-opened':output({'event':'handle_lifecycle','case':name,'iteration':index-warm,'open_ms':open_ms,'close_ms':(time.perf_counter()-closing)*1000})
  except Exception as error:output({'case':name,'status':'failed','error':str(error)})
 if database is not None:close_db(database,connection)
for p in target.parent.glob(target.name+'*'):
 if p.is_file():p.unlink()
