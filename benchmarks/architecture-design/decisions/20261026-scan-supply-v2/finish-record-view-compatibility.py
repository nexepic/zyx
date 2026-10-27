"""Run only selected matching-profile modules after compatibility builds finish."""
from pathlib import Path
import hashlib,json,subprocess,time
R=Path('/workspace/zyx');D=Path(__file__).resolve().parent
profiles=[('hybrid',46209,'build-hybrid-compat','/tmp/zyx-a2-record-view-hybrid-resume-build.log',102),
          ('row',46224,'build-scan-row-production','/tmp/zyx-a2-record-view-row-resume-build.log',110)]
changed=['src/storage/relocatable/index/BTreeIndex.cpp','src/storage/relocatable/index/BTreePage.cpp',
         'src/storage/relocatable/property/HybridPropertyColdRowInspection.cpp']
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
receipts=[]
for profile,pid,build,log,expected in profiles:
    deadline=time.monotonic()+14400
    while Path('/proc',str(pid)).exists():
        assert time.monotonic()<deadline,'Compatibility build did not finish'
        time.sleep(2)
    text=Path(log).read_text()
    assert 'Linking CXX static library libzyx_core.a' in text and 'ninja: build stopped:' not in text,text[-3000:]
    d=D/('record-view-'+profile);d.mkdir(exist_ok=True)
    rows=(R/build/'.ninja_log').read_text().splitlines();checks=[];uncertain=[]
    for source in changed:
        target='CMakeFiles/zyx_core.dir/'+source+'.o'
        entries=[x.split('\t') for x in rows if '\t'+target+'\t' in x];a=entries[-1]
        conservative_start=int(a[2])-(int(a[1])-int(a[0]))*1000000
        safe=conservative_start>(R/source).stat().st_mtime_ns+1000000000
        checks.append(dict(source=source,compile_after_edit=safe))
        if not safe:uncertain.append(R/build/target)
    if uncertain:
        for obj in uncertain:obj.unlink()
        with (d/'fresh-build.log').open('w') as out:
            subprocess.run(['cmake','--build',build,'--target','zyx_core','-j2'],cwd=R,stdout=out,stderr=out,check=True)
    if profile=='hybrid':
        command=json.loads((D/'hybrid/recipes.json').read_text())[-1]
        command=[x.replace(str(D/'hybrid'),str(d)) for x in command]
    else:
        base=json.loads((D/'presence-scan-link.json').read_text());i=base.index(str(R/'build-scan-candidate/libzyx_core.a'))
        compiles=json.loads((D/'record-view-row-index/compile-recipes.json').read_text())
        objects=[a[a.index('-o')+1] for a in compiles]
        hook=R/'benchmarks/architecture-design/decisions/20261026-scan-supply-v1/metadata-row/StorageIO.o'
        command=[base[0],str(hook),*objects]+base[i:]
        command=[x.replace('build-scan-candidate','build-scan-row-production') for x in command]
        command[command.index('-o')+1]=str(d/'modules')
    (d/'link.json').write_text(json.dumps(command,indent=2)+'\n')
    with (d/'link.log').open('w') as out:subprocess.run(command,stdout=out,stderr=out,check=True)
    library=R/build/'libzyx_core.a';before=sha(library)
    command=[str(d/'modules'),'--gtest_output=json:'+str(d/'tests.json')]
    print('START',profile,'selected modules',flush=True)
    with (d/'tests.log').open('w') as out:
        result=subprocess.run(command,stdout=out,stderr=out,check=True,timeout=1800)
    tests=json.loads((d/'tests.json').read_text());assert tests['tests']==expected and tests['failures']==0
    assert sha(library)==before
    receipt=dict(profile=profile,tests=expected,failures=0,command=command,library_sha256=before,
                 source_job_timing_checks=checks,recompiled_uncertain_objects=list(map(str,uncertain)),
                 scope='full matching-profile O3 core; unchanged StorageIO test-hook object only; selected modules, not full suite')
    (d/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n');receipts.append(receipt)
    print('PASS',profile,expected,flush=True)
(D/'record-view-compatibility-receipt.json').write_text(json.dumps(receipts,indent=2)+'\n')
