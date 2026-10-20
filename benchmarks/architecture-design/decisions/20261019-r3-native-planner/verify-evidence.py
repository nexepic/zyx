"""Verify the native planner/private preparation checkpoint, not acceptance."""
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
for path, digest in manifest['files'].items():
    assert hashlib.sha256((root / path).read_bytes()).hexdigest() == digest, path
for path in manifest['original_semantic_tests_unchanged_from_base']:
    assert (root / path).read_bytes() == subprocess.check_output(
        ['git', 'show', manifest['base_commit'] + ':' + path], cwd=root), path
native = (root / 'include/graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp').read_text()
assert not re.search(r'^#include.*(?:benchmarks/|query/execution/)', native, re.M)
adapter = (root / 'benchmarks/architecture-design/property-subblock/ReservedGroupConversion.hpp').read_text()
assert 'native_.visitOwner' in adapter and 'native_.encodeNativeColumn' in adapter
assert 'std::sort' not in adapter and 'static std::optional<ScalarKind> primitive' not in adapter
for profile, count in [('default', 33), ('candidate', 38)]:
    result = json.loads((here / (profile + '-results.json')).read_text())
    assert result['tests'] == count and result['failures'] == result['errors'] == result['disabled'] == 0
    cases = [(s['name'], c) for s in result['testsuites'] for c in s['testsuite']]
    assert len({(s, c['name']) for s, c in cases}) == count
    assert all(c['result'] == 'COMPLETED' and not c.get('failures') for _, c in cases)
    scope = json.loads((here / (profile + '-scope.json')).read_text())
    assert not scope['full_suite'] and len(scope['modules']) == 5
    preparation = [c for s, c in cases if s == 'HybridPropertyConversionPreparationTest']
    assert len(preparation) == (1 if profile == 'default' else 6)
for lang in ['en', 'zh']:
    doc = (root / f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
    counts = re.findall(r'^\| R[0-7].*\| (\d)/3 \|$', doc, re.M)
    assert len(counts) == 8 and sum(map(int, counts)) == 13
print('Verified one classifier, unchanged semantic tests, default33/33, isolated38/38 and13/24')
