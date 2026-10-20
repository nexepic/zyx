"""Check the uncommitted namespace binding checkpoint, not stage acceptance."""
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
path = manifest['original_public_contract_tests_unchanged_from_base']
assert (root / path).read_bytes() == subprocess.check_output(
    ['git', 'show', manifest['base_commit'] + ':' + path], cwd=root)
for profile, count in [('default', 42), ('candidate', 52)]:
    result = json.loads((here / (profile + '-results.json')).read_text())
    assert result['tests'] == count and result['failures'] == result['errors'] == result['disabled'] == 0
    cases = [(s['name'], c) for s in result['testsuites'] for c in s['testsuite']]
    assert len({(s, c['name']) for s, c in cases}) == count
    assert all(c['result'] == 'COMPLETED' and not c.get('failures') for _, c in cases)
    scope = json.loads((here / (profile + '-scope.json')).read_text())
    assert not scope['full_suite'] and len(scope['modules']) == 3
    assert any(c['name'] == 'OrdinaryScalarStagingCannotClaimAnyReservedFieldNamespace' for _, c in cases)
    if profile == 'candidate':
        assert any(c['name'] == 'DescriptorNamespaceCannotBindAnotherRoleOrAnOrdinaryIndexBeforePreparation'
                   for _, c in cases)
for lang in ['en', 'zh']:
    doc = (root / f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
    counts = re.findall(r'^\| R[0-7].*\| (\d)/3 \|$', doc, re.M)
    assert len(counts) == 8 and sum(map(int, counts)) == 13
    spec = (root / f'docs/apps/docs/content/docs/{lang}/zyx/architecture/native-single-file-storage-plan.mdx').read_text()
    assert '0x8000000000000030' in spec and '0x8000000000000037' in spec
    assert 'POK_HYBRID_PROPERTY=16' in spec
print('Verified namespace checkpoint, unchanged public contracts, default42/42, isolated52/52 and13/24')
