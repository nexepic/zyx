from pathlib import Path
from collections import Counter
from functools import lru_cache
import csv,json
ROOT=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume')
SCAN=['label_scan_filter','all_nodes_property_filter','label_multi_property_filter','relationship_type_scan','relationship_property_filter','one_hop_expand','two_hop_expand','aggregation_group_by','aggregation_count_by_group','topk_property_sort','multi_key_group','distinct_tuple','empty_count']
INDEXED=['point_lookup_indexed','point_lookup_missing','property_equality_indexed','property_range_indexed','missing_point']
RETRIEVAL=['point_node_fetch_by_id','point_edge_fetch_by_endpoints','batch_node_fetch_100','one_hop_fetch_neighbor_ids','one_hop_fetch_neighbor_records','property_index_fetch_users_by_country','range_index_fetch_user_projection','relationship_property_fetch','graph_full_point','graph_full_neighbors']
COHORT=['cohort_filter','cohort_group','cohort_point','cohort_missing','cohort_full_point','cohort_full_scan','cohort_gather']
WRITES=['property_set','property_noop','edge_create','edge_create_delete','edge_batch_100','edge_batch_1000','edge_batch_10000','steady_property']
CWRITES=['cohort_set','cohort_noop','cohort_remove']
def cell(v):
 if v is None:return ['null',None]
 if isinstance(v,bool):return ['boolean',v]
 if isinstance(v,int):return ['integer',v]
 if isinstance(v,float):return ['double',v]
 if isinstance(v,str):return ['string',v]
 raise TypeError(type(v))
def normalized(value):
 if isinstance(value,list):
  if len(value)==2 and value[0]=='double' and type(value[1]) in [int,float]:return ['double',float(value[1])]
  return [normalized(x) for x in value]
 if isinstance(value,dict):return {k:normalized(x) for k,x in value.items()}
 return value
def canonical(rows):return Counter(json.dumps(normalized(x),sort_keys=True,separators=(',',':')) for x in rows)
@lru_cache(None)
def data(scale):
 source=ROOT/f'r7-inputs/{scale}'
 raw=list(csv.DictReader((source/'users.csv').open()));users={u['id']:{'id':u['id'],'age':int(u['age']),'country':u['country'],'score':float(u['score'])} for u in raw}
 countries=Counter(u['country'] for u in users.values());groups=Counter((u['country'],u['age']) for u in users.values())
 follows=list(csv.DictReader((source/'follows.csv').open()));first=[e for e in follows if e['src']=='user-000001'];hops=Counter(e['dst'] for e in first)
 counts=json.loads((source/'manifest.json').read_text())['counts']
 return dict(users=users,countries=countries,groups=groups,first=first,hops=hops,counts=counts,
 numeric=sum(u['score']>=900 for u in users.values())+sum(float(p['score'])>=900 for p in csv.DictReader((source/'posts.csv').open())),
 weighted=sum(int(e['weight'])==1 for e in follows),targets=Counter(e['dst'] for e in follows if int(e['weight'])==1),twohop=sum(hops.get(e['src'],0) for e in follows))
def require(value,reason):
 if not value:raise AssertionError(reason)
def node_properties(v,label,properties):
 require(isinstance(v,list) and len(v)==2 and v[0]=='node','owning Node type')
 require(type(v[1]['internal_id']) is int and (v[1]['internal_id']>=0 and type(v[1].get('table')) is int and v[1]['table']>=0 if v[1].get('identity_kind')=='kuzu_offset' else v[1]['internal_id']>0),'valid engine-scoped Node identity')
 require(v[1]['labels']==[label],'complete labels')
 require(v[1]['properties']=={k:cell(x) for k,x in properties.items()},'complete Node property payload')
def random_body(owner,field):
 mask=(1<<64)-1;state=owner*1009+field*97+1;chars=[]
 for _ in range(8192):
  state=(state^(state<<13))&mask;state=(state^(state>>7))&mask;state=(state^(state<<17))&mask;chars.append(chr(33+state%90))
 return ''.join(chars)
def check(name,rows,scale='medium',iteration=0):
 expected=None;ordered=False
 if name.startswith('cohort_'):
  if name=='cohort_filter':expected=[[cell(257)]]
  elif name=='cohort_group':expected=[[cell(i+1),cell(1)] for i in range(1,513)];ordered=True
  elif name=='cohort_point':expected=[[cell(65)]]
  elif name=='cohort_missing':expected=[]
  elif name=='cohort_gather':expected=[[cell(2),cell(65)],[cell(0),cell(None)],[cell(2),cell(65)],[cell(513),cell(576)]];ordered=True
  elif name in ['cohort_set','cohort_noop','cohort_remove']:expected=[[cell(777 if name=='cohort_set' else 65 if name=='cohort_noop' else None)]]
  elif name in ['cohort_full_point','cohort_full_scan']:
   ids=[1] if name=='cohort_full_point' else list(range(1,513));require(len(rows)==len(ids),'cohort full cardinality')
   for row,i in zip(rows,ids):require(len(row)==1,'cohort full width');node_properties(row[0],'Owner',{f'p{k}':i+k for k in range(1,65)})
   return
 elif name.startswith('large_'):
  if name=='large_full':require(len(rows)==1 and len(rows[0])==1,'large full cardinality');node_properties(rows[0][0],'Large',{'ordinal':1,**{chr(96+k):random_body(1,k) for k in range(1,4)}});return
  expected=[[cell(-1 if name=='large_patch' else 1)]]
 elif name.startswith('skew_'):
  edges=[]
  for i in range(512):
   if i%3:continue
   src=0 if i<256 else i*17%32;dst=src if i%17==0 else (i*7+3)%32;edges.append((src,dst))
  degree=Counter(a for a,b in edges)
  count=sum(a==0 for a,b in edges) if name=='skew_out' else sum(a==0 or b==0 for a,b in edges) if name=='skew_both' else sum(degree[b] for a,b in edges if a==0)
  expected=[[cell(count)]]
 elif name in WRITES:
  if name=='property_set':expected=[[cell(-1.0)]]
  elif name=='steady_property':expected=[[cell(-1000.0-iteration)]]
  elif name=='property_noop':expected=[[cell(data(scale)['users']['user-000006']['score'])]]
  else:expected=[[cell(int(name.rsplit('_',1)[-1]) if name.startswith('edge_batch_') else 1)]]
 else:
  d=data(scale);u=d['users'];count={'label_scan_filter':d['countries']['CN'],'all_nodes_property_filter':d['numeric'],'label_multi_property_filter':sum(x['country']=='CN' and x['age']>=30 for x in u.values()),'relationship_type_scan':d['counts']['follows'],'relationship_property_filter':d['weighted'],'one_hop_expand':len(d['first']),'two_hop_expand':d['twohop'],'aggregation_group_by':len(d['countries']),'distinct_tuple':len(d['groups']),'empty_count':0,'point_lookup_indexed':1,'point_lookup_missing':0,'property_equality_indexed':d['countries']['CN'],'property_range_indexed':sum(30<=x['age']<40 for x in u.values())}
  if name in count:expected=[[cell(count[name])]]
  elif name=='aggregation_count_by_group':expected=[[cell(k),cell(v)] for k,v in d['countries'].items()]
  elif name=='multi_key_group':expected=[[cell(k[0]),cell(k[1]),cell(v)] for k,v in sorted(d['groups'].items())];ordered=True
  elif name=='missing_point':expected=[]
  elif name=='topk_property_sort':
   require(len(rows)==100 and all(len(row)==1 and row[0][0]=='string' for row in rows),'TopK shape')
   ids=[row[0][1] for row in rows];require(len(set(ids))==100 and all(i in u for i in ids),'TopK identities')
   scores=[u[i]['score'] for i in ids];require(scores==sorted(scores,reverse=True) and min(scores)>=sorted((x['score'] for x in u.values()),reverse=True)[99],'TopK complete ordered scores');return
  elif name=='point_node_fetch_by_id':x=u['user-000001'];expected=[[cell(x[k]) for k in ['id','age','country','score']]]
  elif name=='point_edge_fetch_by_endpoints':expected=[[cell(int(e['weight']))] for e in d['first'] if e['dst']=='user-000006']
  elif name=='one_hop_fetch_neighbor_ids':expected=[[cell(e['dst'])] for e in d['first']]
  elif name=='one_hop_fetch_neighbor_records':expected=[[cell(u[e['dst']][k]) for k in ['id','age','country','score']] for e in d['first']]
  elif name in ['batch_node_fetch_100','property_index_fetch_users_by_country','range_index_fetch_user_projection']:
   require(len(rows)==100,'limited full rows cardinality');ids=[]
   for row in rows:
    identifier=row[0][1];require(identifier in u,'limited result identity');ids.append(identifier);x=u[identifier]
    keys=['id','age','score'] if name=='property_index_fetch_users_by_country' else ['id','age','country','score']
    require(row==[cell(x[k]) for k in keys],'limited full payload')
    if name=='property_index_fetch_users_by_country':require(x['country']=='CN','country predicate')
    if name=='range_index_fetch_user_projection':require(30<=x['age']<40,'age range predicate')
   require(len(set(ids))==100,'limited duplicate identities');return
  elif name=='relationship_property_fetch':
   require(len(rows)==100,'relationship limited cardinality');targets=Counter()
   for row in rows:require(len(row)==2 and row[0]==cell(1) and row[1][0]=='string','relationship full payload');targets[row[1][1]]+=1
   require(all(n<=d['targets'][key] for key,n in targets.items()),'relationship multiplicity');return
  elif name in ['graph_full_point','graph_full_neighbors']:
   ids=['user-000001'] if name=='graph_full_point' else sorted(e['dst'] for e in d['first']);require(len(rows)==len(ids),'full Node cardinality')
   for row,i in zip(rows,ids):require(len(row)==1,'full Node width');node_properties(row[0],'User',u[i])
   return
 require(expected is not None,'missing oracle case '+name)
 require(rows==expected if ordered else canonical(rows)==canonical(expected),'complete typed result mismatch '+name)

def check_event(name,event,scale='medium',require_post=False):
 check(name,event['rows'],scale,event.get('iteration',0))
 if not require_post or name=='steady_property':return
 require('postcondition_rows' in event,'missing persisted postcondition '+name)
 if name.startswith('edge_batch_'):
  original=sum(e['dst']=='user-000002' and int(e['weight'])==1 for e in data(scale)['first'])
  require(event['postcondition_rows']==[[cell(original+int(name.rsplit('_',1)[-1]))]],'persisted parallel edge count')
 elif name in ['edge_create','edge_create_delete']:
  require(event['postcondition_rows']==[[cell(1 if name=='edge_create' else 0)]],'persisted create/delete marker')
 else:require(canonical(event['postcondition_rows'])==canonical(event['rows']),'persisted scalar result')
