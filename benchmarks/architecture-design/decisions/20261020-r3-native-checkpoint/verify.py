from pathlib import Path
import hashlib,json
base=Path(__file__).resolve().parent
root=base.parents[3]
manifest=json.loads((base/'source-manifest.json').read_text())
for name,identity in manifest['files'].items():
    data=(root/name).read_bytes()
    assert hashlib.sha256(data).hexdigest()==identity['sha256'],name
    assert len(data)==identity['bytes'],name
summary=json.loads((base/'validation.json').read_text())
for profile,expected in summary['profiles'].items():
    seen=set();suites=set();modules=set()
    for part in ['main','additional']:
        result=json.loads((base/f'{profile}-{part}-results.json').read_text())
        scope=json.loads((base/f'{profile}-{part}-scope.json').read_text())
        assert scope['full_suite'] is False
        assert all(result[k]==0 for k in ['failures','disabled','errors'])
        assert result['tests']==expected['parts'][part]
        modules.update(scope['modules'])
        for suite in result['testsuites']:
            suites.add(suite['name'])
            for case in suite['testsuite']:
                key=(suite['name'],case['name'])
                assert key not in seen,key
                assert case['status']=='RUN' and case['result']=='COMPLETED',key
                seen.add(key)
    assert (len(seen),len(suites),len(modules))==(expected['tests'],expected['suites'],expected['modules'])
asan=json.loads((base/'asan-evidence.json').read_text())['variants']
assert asan['old']['returncode']==86 and asan['fixed']['returncode']==0
assert 'heap-buffer-overflow' in (base/'asan-old.log').read_text()
assert '[  PASSED  ] 1 test.' in (base/'asan-fixed.log').read_text()
print('Source identities, scoped tests and ASAN controls verified.')
