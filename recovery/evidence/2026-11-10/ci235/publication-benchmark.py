"""Alternating Linux Debug regression microcomparison, not a production benchmark."""
import json, math, os, re, statistics, subprocess, time
from pathlib import Path

root = Path('/workspace/zyx-ci235-publication-benchmark')
root.mkdir(exist_ok=True)
profiles = root / 'profiles'; profiles.mkdir(exist_ok=True)
binaries = {'before': Path('/workspace/shared/zyx-ci235-baseline-test'),
            'after': Path('/workspace/zyx-ci-reconstructed/buildDir/zyx_test_suite')}
selected = ':'.join([
    'LogicalVectorPropertyBatchTest.SelectedColumnsShareRetainedResultBudgetAndKeepHitOrder',
    'LogicalVectorBufferedSortTest.SelectedPropertyGatherPreservesThreeHundredRowsAcrossBoundedReads',
    'LogicalVectorPropertyBatchTest.CompleteEntityGatherAndRowsShareBudgetAcrossBoundedBatches'])
records = []
def run(name, label, repeats):
    env = os.environ.copy(); env['ZYX_TEST_TERSE_OUTPUT'] = '1'
    env['ZYX_CI_CASE_TRACE'] = '1'
    env['LLVM_PROFILE_FILE'] = str(profiles / f'{label}-{name}-%p.profraw')
    env.pop('GTEST_TOTAL_SHARDS', None); env.pop('GTEST_SHARD_INDEX', None)
    env.pop('ZYX_CI_BUDGET_TRACE', None); env.pop('ZYX_CI_RSS_TRACE', None)
    started = time.perf_counter()
    result = subprocess.run([str(binaries[name]), '--gtest_filter=' + selected,
        '--gtest_repeat=' + str(repeats)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        cwd='/workspace/zyx-ci-reconstructed', env=env, timeout=300)
    elapsed = time.perf_counter() - started
    (root / f'{label}-{name}.log').write_bytes(result.stdout)
    text = re.sub(rb'\x1b\[[0-9;]*m', b'', result.stdout).decode(errors='replace')
    passed = [int(value) for value in re.findall(r'Passed:\s+(\d+)', text)]
    ended = re.findall(r'CI_CASE_END (\S+) failed=0', text)
    assert result.returncode == 0 and passed == [3] and len(ended) == 3 * repeats, (name, label, result.returncode, passed, len(ended))
    assert all(ended.count(case) == repeats for case in selected.split(':'))
    return {'binary':name, 'round':label, 'repeats':repeats, 'passed':len(ended),
            'seconds':elapsed, 'exit':result.returncode}
for name in binaries: run(name, 'warmup', 5)
for pair in range(6):
    for name in (('before','after') if pair % 2 == 0 else ('after','before')):
        record = run(name, pair, 100); records.append(record); print(record, flush=True)
delta = [next(r['seconds'] for r in records if r['round']==i and r['binary']=='after') -
         next(r['seconds'] for r in records if r['round']==i and r['binary']=='before') for i in range(6)]
margin = 2.570582 * statistics.stdev(delta) / math.sqrt(6)
summary = {'scope':'Linux Debug -O0 -g0 with coverage; fake-backend query regression microcomparison',
           'not_production_or_native_platform_benchmark':True, 'selected':selected,
           'records':records, 'paired_after_minus_before_mean_seconds':statistics.mean(delta),
           'paired_95pct_t_interval_seconds':[statistics.mean(delta)-margin,statistics.mean(delta)+margin]}
(root/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps({k:v for k,v in summary.items() if k!='records'},indent=2))
