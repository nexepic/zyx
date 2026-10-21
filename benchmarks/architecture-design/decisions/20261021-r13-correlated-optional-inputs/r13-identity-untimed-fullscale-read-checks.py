from pathlib import Path
import csv,json,subprocess,resource,shutil,hashlib,time,sys
from collections import Counter
r=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume')
binary=r/'r13-row-result-contract'
def limits():
    resource.setrlimit(resource.RLIMIT_AS,(6*1024**3,6*1024**3))
    resource.setrlimit(resource.RLIMIT_FSIZE,(2*1024**3,2*1024**3))
def integer(value):return ['integer',value]
def string(value):return ['string',value]
def canonical(rows):return Counter(json.dumps(row,sort_keys=True,separators=(',',':')) for row in rows)
report={'purpose':'Untimed complete typed results on registered medium and holdout inputs','timing_benchmark':False,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'cases':[]}
scales=sys.argv[1:] or ['medium','holdout']
prepared={c['scale']:c for c in json.loads((r/'r43-identity-untimed-preparation.json').read_text())['cases']}
for scale in scales:
    if prepared.get(scale,{}).get('returncode')!=0:raise RuntimeError('Scale has no completed successful preparation')
    source=r/f'r7-inputs/{scale}';original=r/f'r7-inputs/{scale}-r43-identity.zyx';private=r/f'r7-inputs/{scale}-r43-identity-correctness.zyx'
    if scale == 'medium':
        assert private.exists()
    else:
        if private.exists():raise RuntimeError('fresh holdout clone required')
        shutil.copyfile(original,private)
    users=list(csv.DictReader((source/'users.csv').open()));posts=list(csv.DictReader((source/'posts.csv').open()))
    manifest=json.loads((source/'manifest.json').read_text());counts=manifest['counts'];countries=Counter(u['country'] for u in users)
    groups=Counter((u['country'],int(u['age'])) for u in users)
    first_hop=Counter(row['dst'] for row in csv.DictReader((source/'follows.csv').open()) if row['src']=='user-000001')
    onehop=sum(first_hop.values())
    twohop=sum(first_hop.get(row['src'],0) for row in csv.DictReader((source/'follows.csv').open()))
    weighted=sum(int(row['weight'])==1 for row in csv.DictReader((source/'follows.csv').open()))
    cn={u['id'] for u in users if u['country']=='CN'}
    broad=sum(row['src'] in cn for row in csv.DictReader((source/'follows.csv').open()))
    cases=[
        ('all_nodes', 'MATCH (n) RETURN count(n)', [[integer(counts['users']+counts['posts']+counts['tags'])]],False),
        ('users','MATCH (u:User) RETURN count(u)',[[integer(len(users))]],False),
        ('string_filter',"MATCH (u:User) WHERE u.country = 'CN' RETURN count(u)",[[integer(countries['CN'])]],False),
        ('numeric_filter','MATCH (n) WHERE n.score >= 900.0 RETURN count(n)',[[integer(sum(float(n['score'])>=900 for n in users+posts))]],False),
        ('multi_filter',"MATCH (u:User) WHERE u.country = 'CN' AND u.age >= 30 RETURN count(u)",[[integer(sum(u['country']=='CN' and int(u['age'])>=30 for u in users))]],False),
        ('typed_edges','MATCH ()-[r:FOLLOWS]->() RETURN count(r)',[[integer(counts['follows'])]],False),
        ('edge_filter','MATCH ()-[r:FOLLOWS]->() WHERE r.weight = 1 RETURN count(r)',[[integer(weighted)]],False),
        ('distinct_country','MATCH (u:User) RETURN count(DISTINCT u.country)',[[integer(len(countries))]],False),
        ('single_key_groups','MATCH (u:User) RETURN u.country, count(*)',[[string(c),integer(n)] for c,n in countries.items()],False),
        ('multiple_key_groups','MATCH (u:User) RETURN u.country, u.age, count(*)',[[string(c),integer(a),integer(n)] for (c,a),n in groups.items()],False),
        ('distinct_tuples','MATCH (u:User) RETURN count(DISTINCT [u.country, u.age])',[[integer(len(groups))]],False),
        ('empty_aggregate','MATCH (u:User) WHERE u.age < 0 RETURN count(u)',[[integer(0)]],False),
        ('missing_aggregate','MATCH (u:User) RETURN count(u.absent_property)',[[integer(0)]],False),
        ('one_hop',"MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN count(v)",[[integer(onehop)]],False),
        ('two_hop',"MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(:User)-[:FOLLOWS]->(v:User) RETURN count(v)",[[integer(twohop)]],False),
        ('broad_expand',"MATCH (u:User {country: 'CN'})-[:FOLLOWS]->(v:User) RETURN count(v)",[[integer(broad)]],False),
        ('ordered_topk','MATCH (u:User) RETURN u.id ORDER BY u.score DESC, u.id ASC LIMIT 100',[[string(u['id'])] for u in sorted(users,key=lambda u:(-float(u['score']),u['id']))[:100]],True),
    ]
    by_id={u['id']:u for u in users}
    def node_properties(u):
        return {'id':string(u['id']),'age':integer(int(u['age'])),'country':string(u['country']),'score':['double',float(u['score'])]}
    cases += [
        ('full_point',"MATCH (u:User {id: 'user-000001'}) RETURN u",[node_properties(by_id['user-000001'])],True),
        ('full_neighbors',"MATCH (:User {id: 'user-000001'})-[:FOLLOWS]->(v:User) RETURN v ORDER BY v.id",
            [node_properties(by_id[target]) for target,count in sorted(first_hop.items()) for _ in range(count)],True),
        ('duplicate_missing_gather',"UNWIND ['user-000003', 'user-000001', 'not-found', 'user-000001'] AS probe OPTIONAL MATCH (u:User {id: probe}) RETURN probe, u.age",
            [[string(probe), integer(int(by_id[probe]['age'])) if probe in by_id else ['null',None]]
                for probe in ['user-000003','user-000001','not-found','user-000001']],False),
    ]
    deadline=time.monotonic()+1800
    for name,query,expected,ordered in cases:
        if scale == 'medium' and name != 'duplicate_missing_gather':continue
        output=r/f'r13-identity-{scale}-{name}-typed-rows.json';errors=r/f'r13-identity-{scale}-{name}.stderr'
        with output.open('w') as out,errors.open('w') as err:
            p=subprocess.run([str(binary),'query',str(private),query],stdout=out,stderr=err,preexec_fn=limits,timeout=max(1,deadline-time.monotonic()))
        actual=json.loads(output.read_text()) if p.returncode==0 else None
        compared=actual
        entity_valid=True
        if p.returncode==0 and name in ['full_point','full_neighbors']:
            entity_valid=all(len(row)==1 and row[0][0]=='node' and type(row[0][1]['internal_id']) is int and
                row[0][1]['internal_id']>0 and row[0][1]['label']=='User' and row[0][1]['labels']==['User'] for row in actual)
            compared=[row[0][1]['properties'] for row in actual] if entity_valid else None
        passed=p.returncode==0 and entity_valid and (compared==expected if ordered else canonical(compared)==canonical(expected))
        case={'scale':scale,'name':name,'query':query,'returncode':p.returncode,'passed':passed,'ordered':ordered,'rows':len(actual) if actual is not None else None,'expected':expected}
        report['cases'].append(case);(r/('r13-identity-untimed-fullscale-read-results-'+','.join(scales)+'.json')).write_text(json.dumps(report,indent=2)+'\n')
        print(scale,name,passed,flush=True)
        if not passed:raise SystemExit(1)
