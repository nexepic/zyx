from pathlib import Path
import concurrent.futures,subprocess,os,re,json
root=Path('/workspace/zyx-journal-race-evidence');repo=Path('/workspace/zyx-ci-reconstructed')
def run(i):
 tmp=root/f'repeat-{i}-tmp';tmp.mkdir(exist_ok=True);env=os.environ.copy();env.update(TMPDIR=str(tmp),ZYX_CI_CASE_TRACE='1',LLVM_PROFILE_FILE=str(root/f'repeat-{i}-%p.profraw'))
 log=root/f'repeat-{i}.log'
 with log.open('w') as out:r=subprocess.run([str(repo/'buildDir/zyx_test_suite'),'--gtest_filter=RelocatableStorageBackendSchemaTest.LargeValueMaintenanceJournalResumesAcrossReopenAtEveryDurablePhase','--gtest_repeat=25'],cwd=repo,env=env,stdout=out,stderr=subprocess.STDOUT)
 s=log.read_text();return {'process':i,'exit':r.returncode,'begins':s.count('CI_CASE_BEGIN'),'ends':s.count('CI_CASE_END'),'failed':len(re.findall(r'CI_CASE_END \S+ failed=1',s))}
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:results=list(pool.map(run,range(4)))
(root/'repeat-results.json').write_text(json.dumps(results,indent=2)+'\n');print(results,flush=True)
assert all(r['exit']==0 and r['begins']==25 and r['ends']==25 and not r['failed'] for r in results)
