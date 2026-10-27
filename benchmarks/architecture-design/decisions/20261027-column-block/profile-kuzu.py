from pathlib import Path
import json,hashlib,kuzu,re
R=Path('/workspace/zyx');N=Path(__file__).resolve().parent;source=R/'benchmarks/architecture-design/results/20261024-work-scan/medium.kuzu';O=N/'kuzu-profile.json';assert not O.exists()
sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest();before=sha(source)
db=kuzu.Database(str(source),read_only=True,buffer_pool_size=64*1024**2,max_num_threads=1,max_db_size=2*1024**3);c=kuzu.Connection(db,num_threads=1)
text=(N.parent/'20261023-core-cost-acceptance/public-probe.cpp').read_text()
plans={name:{'query':json.loads(chr(34)+sql+chr(34))} for name,sql in re.findall(r'if\(name=="([^"]+)"\)return "((?:[^"\\]|\\.)*)";',text)};out={}
for case in ('all_nodes_property_filter','label_scan_filter','multi_key_group'):
 q=plans[case]['query'];result=c.execute('PROFILE '+q);rows=[]
 while result.has_next():rows.append(result.get_next())
 result.close();out[case]={'query':q,'profile':rows}
c.close();db.close();assert sha(source)==before
O.write_text(json.dumps({'kuzu_version':kuzu.__version__,'source_sha256':before,'source_unchanged':True,'scope':'PROFILE diagnostic only; instrumented timings are not acceptance or comparable to unobserved ZYX latency','queries':out},ensure_ascii=False,indent=2)+'\n')
print('Saved3 Kuzu execution profiles; source unchanged')
