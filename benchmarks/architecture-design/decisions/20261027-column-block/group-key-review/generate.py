import hashlib, json, os, pathlib, subprocess, sys
here=pathlib.Path(__file__).resolve().parent
binary=pathlib.Path('/tmp/zyx-r1-scan-tests')
assert binary.is_file()
results=[]
for rows in (512,1536):
    for mode in range(5):
        output=pathlib.Path(f'/tmp/zyx-borrowed-frozen-{rows}-{mode}.dat')
        if rows==512 and mode==0 and output.is_file():
            log=pathlib.Path('/tmp/zyx-borrowed-fixture3-run.log')
            status=0
        else:
            env=os.environ.copy();env.update(ZYX_GROUP_ROWS=str(rows), ZYX_GROUP_MODE=str(mode), ZYX_GROUP_OUTPUT=str(output), LLVM_PROFILE_FILE=f'/tmp/zyx-group-fixture-{rows}-{mode}-%p.profraw')
            log=here/f'fixture-{rows}-{mode}.log'
            with log.open('w') as out:
                status=subprocess.run([str(binary),'--gtest_also_run_disabled_tests','--gtest_filter=ScanReadProofTest.DISABLED_BorrowedGroupPersistentFixture'],env=env,stdout=out,stderr=subprocess.STDOUT).returncode
        record={'rows':rows,'mode':mode,'returncode':status,'log':str(log),'path':str(output)}
        if status==0 and output.is_file():
            record.update(bytes=output.stat().st_size,sha256=hashlib.sha256(output.read_bytes()).hexdigest())
        results.append(record)
        (here/'fixture-receipt.json').write_text(json.dumps({'fixtures':results},indent=2)+'\n')
        if status: sys.exit(status)
