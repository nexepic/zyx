import collections
import hashlib
import gzip
import json
import os
import re
from pathlib import Path
import subprocess
import xml.etree.ElementTree as ET

root = Path('/workspace/zyx')
binary = root / 'buildDir/zyx_test_suite'
environment = os.environ.copy()
for key in list(environment):
    if key.startswith('ZYX_CI_') or key.startswith('GTEST_'):
        environment.pop(key)
listing = subprocess.check_output([str(binary), '--gtest_list_tests'], env=environment, text=True)
with gzip.open(root / 'recovery/evidence/2026-11-13/run43352142505/discovery.txt.gz', 'wt') as stream:
    stream.write(listing)
discovered = []
suite = None
for line in listing.splitlines():
    name = line.split('#', 1)[0].strip()
    if not name:
        continue
    if not line.startswith(' ') and name.endswith('.'):
        suite = name
    elif line.startswith('  ') and suite:
        discovered.append(suite + name)
assert len(discovered) == len(set(discovered)) and len(discovered) > 10000
disabled = [name for name in discovered if any(part.startswith('DISABLED_') for part in name.split('.'))]
enabled = set(discovered) - set(disabled)
executed, failures, skips, shard_times = [], [], [], []
paths = sorted(Path('/workspace/ci242-full-xml').glob('*.xml'))
assert len(paths) == 4
for path in paths:
    tree = ET.parse(path).getroot()
    shard_times.append(float(tree.get('time', '0')))
    for case in tree.iter('testcase'):
        name = case.get('classname') + '.' + case.get('name')
        if case.get('status') == 'run':
            executed.append(name)
            if case.find('failure') is not None:
                failures.append(name)
            if case.find('skipped') is not None:
                skips.append(name)
counts = collections.Counter(executed)
previous = json.loads((root / 'recovery/evidence/2026-11-11/ci236/full-summary.json').read_text())
result = {
    'source_commit': '457a9c5637bb453f22f4e1e81c4436adbf2f4a1a',
    'code_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip(),
    'binary_sha256': hashlib.file_digest(binary.open('rb'), 'sha256').hexdigest(),
    'discovered': len(discovered), 'enabled': len(enabled), 'disabled': disabled,
    'executed': len(executed), 'distinct': len(counts), 'failures': failures, 'skips': skips,
    'missing': sorted(enabled - counts.keys()), 'extra': sorted(counts.keys() - enabled),
    'duplicates': {name: count for name, count in counts.items() if count != 1},
    'stress_counts': {name: counts[name] for name in previous['stress_counts']},
    'shard_times_seconds': shard_times,
}
result['launcher'] = json.loads(Path('/workspace/ci242-full-shards.json').read_text())
console = re.sub(r'\x1b\[[0-9;]*m', '', Path('/workspace/ci242-full.log').read_text())
total = [line for line in console.splitlines() if line.startswith('TOTAL')][-1]
percentages = [float(value) for value in re.findall(r'([0-9]+\.[0-9]+)%', total)]
assert len(percentages) == 4
result['coverage'] = dict(zip(['regions_percent', 'functions_percent', 'lines_percent', 'branches_percent'], percentages))
output = root / 'recovery/evidence/2026-11-13/run43352142505/full-summary.json'
output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
assert not any(result[name] for name in ['failures', 'skips', 'missing', 'extra', 'duplicates'])
assert all(count == 1 for count in result['stress_counts'].values())

assert result['discovered'] == 11127 and result['executed'] == 11107
subprocess.run(['git','diff','457a9c5637bb453f22f4e1e81c4436adbf2f4a1a','--exit-code','--','src','include','tests','scripts'], cwd=root,check=True)
assert result['binary_sha256'] == json.loads((root/'recovery/evidence/2026-11-13/run43352142505/source-validation.json').read_text())['binary_sha256']
