"""Check this uncommitted source checkpoint and raw module evidence."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

here = Path(__file__).resolve().parent
root = here.parents[3]
manifest = json.loads((here / 'source-manifest.json').read_text())
assert manifest['source_basis'] == 'uncommitted working tree'
assert manifest['code_checkpoint'] is None and not manifest['accepted_progress_changed']
for name, digest in manifest['files'].items():
    assert hashlib.sha256((root / name).read_bytes()).hexdigest() == digest, name
unchanged = manifest['retired_code_rejection_test_unchanged_from_base']
assert (root / unchanged).read_bytes() == subprocess.check_output(
    ['git', 'show', manifest['base_commit'] + ':' + unchanged], cwd=root)
for profile, count in [('default', 108), ('candidate', 122)]:
    result = json.loads((here / (profile + '-results.json')).read_text())
    assert result['tests'] == count and result['failures'] == 0
    assert result['errors'] == result['disabled'] == 0
    cases = [(suite['name'], case) for suite in result['testsuites'] for case in suite['testsuite']]
    assert len({(suite, case['name']) for suite, case in cases}) == count
    assert all(case['result'] == 'COMPLETED' and not case.get('failures') for _, case in cases)
    scope = json.loads((here / (profile + '-scope.json')).read_text())
    assert not scope['full_suite'] and len(scope['modules']) == 7
    assert any(case['name'] == 'TreePoolEpochAuthenticatesExactlyAndIsIndependentOfMetadataVisibility'
               for _, case in cases)
initial = json.loads((here / 'initial-reserved-code-failure.json').read_text())
assert initial['tests'] == 104 and initial['failures'] == 1
failed = [(s['name'], c['name']) for s in initial['testsuites'] for c in s['testsuite'] if c.get('failures')]
assert failed == [('RelocatableSharedPagePoolTest', 'OwnerHeaderRoundTripsAndRejectsCorruption')]
for lang in ['en', 'zh']:
    doc = (root / f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
    counts = re.findall(r'^\| R[0-7].*\| (\d)/3 \|$', doc, re.M)
    assert len(counts) == 8 and sum(map(int, counts)) == 13
print('Verified source, unchanged retired-code rejection, final108/122 and derived13/24')
