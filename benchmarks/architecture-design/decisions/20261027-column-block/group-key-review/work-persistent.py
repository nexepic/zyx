import json,os,pathlib,subprocess,sys
here=pathlib.Path(__file__).resolve().parent
fixtures=json.loads((here/'fixture-receipt.json').read_text())['fixtures'];results=[]
for f in fixtures:
 env=os.environ.copy();env.update(ZYX_GROUP_ROWS=str(f['rows']),ZYX_GROUP_MODE=str(f['mode']),ZYX_GROUP_INPUT=f['path'],LLVM_PROFILE_FILE=f"/tmp/zyx-group-work-{f['rows']}-{f['mode']}-%p.profraw")
 log=here/f"work-{f['rows']}-{f['mode']}.log"
 with log.open('w') as out:
  p=subprocess.run(['/tmp/zyx-r1-scan-tests','--gtest_also_run_disabled_tests','--gtest_filter=ScanReadProofTest.DISABLED_BorrowedGroupPersistentWork'],env=env,stdout=out,stderr=subprocess.STDOUT)
 lines=[x for x in log.read_text().splitlines() if x.startswith('GROUP_WORK ')]
 results.append({'rows':f['rows'],'mode':f['mode'],'exit':p.returncode,'work':lines,'log':log.name})
 (here/'persistent-work.json').write_text(json.dumps({'scope':'untimed separately instrumented, full result oracle','runs':results},indent=2)+'\n')
 if p.returncode or len(lines)!=2:sys.exit(p.returncode or 1)
