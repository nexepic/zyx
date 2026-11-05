from pathlib import Path
import subprocess,os,json,concurrent.futures
root=Path('/workspace/zyx-ci-fix');build=Path('/workspace/zyx-ci-config');base=Path('/workspace/zyx-ci228-full-asan-core');out=base/'post-full';out.mkdir(exist_ok=True)
env={**os.environ,'LIBRARY_PATH':'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'};units=json.loads((base/'compile-manifest.json').read_text())['units']
rels=['src/query/execution/LogicalPlanReadRuntime.cpp','src/query/execution/LogicalPlanProcedureRuntime.cpp']
records=[]
def compile_one(rel):
 item=next(x for x in units if x['source']==str(root/rel));cmd=item['command'].copy();obj=out/(Path(rel).name+'.o');cmd[cmd.index('-o')+1]=str(obj)
 with obj.with_suffix('.log').open('w') as f:r=subprocess.run(cmd,cwd=build,env=env,stdout=f,stderr=subprocess.STDOUT)
 records.append({'source':rel,'object':str(obj),'command':cmd,'exit':r.returncode});print(Path(rel).name,r.returncode,flush=True);return r.returncode
with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:results=list(pool.map(compile_one,rels))
(out/'manifest.json').write_text(json.dumps({'source_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'base_archive_commit':'ff02643c0d7bdf43e9a0364129982ff3c7c7ed33','production_source_diff':['src/query/execution/LogicalPlanProcedureRuntime.cpp','src/query/execution/read_runtime/PlanInspectionAndBatching.inc'],'units':records},indent=2))
if any(results):raise SystemExit(1)
s=Path('/tmp/zyx-ci228-full-asan-tests-relink.py').read_text();s=s.replace("results=[(0,str(out/('tests/'+Path(rel).stem+'.o'))) for rel in rels]","results=[(0,str(out/('tests/'+Path(rel).stem+'.o'))) for rel in sorted(rels)]");needle="args[1:1]=[*[x[1] for x in results],'/tmp/zyx-tree-focused/gtest_main.o']";assert needle in s;s=s.replace(needle,"args[1:1]=[*[x[1] for x in results],str(out/'post-full/LogicalPlanReadRuntime.cpp.o'),str(out/'post-full/LogicalPlanProcedureRuntime.cpp.o'),'/tmp/zyx-tree-focused/gtest_main.o']");s=s.replace("out/'native_tests'","out/'post-full-native-tests'");exec(compile(s,'<post-link>','exec'))
