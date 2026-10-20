"""Verify this source checkpoint and raw necessary-module results, not acceptance."""
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
for profile, count in [('default', 60), ('candidate', 86)]:
    result = json.loads((here / (profile + '-results.json')).read_text())
    assert result['tests'] == count and result['failures'] == result['errors'] == result['disabled'] == 0
    cases = [(s['name'], c) for s in result['testsuites'] for c in s['testsuite']]
    assert len({(s, c['name']) for s, c in cases}) == count
    assert all(c['result'] == 'COMPLETED' and not c.get('failures') for _, c in cases)
    scope = json.loads((here / (profile + '-scope.json')).read_text())
    assert not scope['full_suite'] and len(scope['modules']) == 6
    assert any(c['name'] == 'FormatFlagControlsTheTypedOrdinaryRequestBeforeAnyPublication' for _, c in cases)
    assert any(c['name'] == 'NativeBulkPlanRetainsItsAccountOutsideTheAllocationScopeAndAcrossCopies' for _, c in cases)
    assert any(c['name'] == 'NativeBulkPlanningRefusesItsBudgetAndRefundsWithoutPublishing' for _, c in cases)
    if profile == 'candidate':
        assert any(c['name'] == 'PhysicalDestinationExhaustionRefundsAfterPartialPrivateBindingAndCanRetry' for _, c in cases)
        assert any(c['name'] == 'CancellationAfterFinalRootPreparationRefundsEveryPrivateImageAndCanRetry' for _, c in cases)
        assert any(c['name'] == 'CompleteBoundedDenseGroupPreparesAllDirectoryPlansWithinTheConversionAccount' for _, c in cases)
        assert any(c['name'] == 'RowConversionRejectsAnExistingDirectoryEvenWhenItsNewDirectoryWouldBeAbsent' for _, c in cases)
        assert any(c['name'] == 'ConversionWorkspaceObeysTheExistingParentAccountAndRefundsItsCredit' for _, c in cases)
        assert any(c['name'] == 'LargeBackedRowIsAuthenticatedAndPreservedWhenInlineFieldPreparationRefuses' for _, c in cases)
for profile in ['default', 'candidate']:
    initial = (here / ('initial-' + profile + '-build.log')).read_text()
    assert 'clang frontend command failed' in initial and 'build stopped' in initial
assert len(set(manifest['current_batch'])) == 10 and 'R7.3' not in manifest['current_batch']
for lang in ['en', 'zh']:
    doc = (root / f'docs/apps/docs/content/docs/{lang}/zyx/architecture/optimization.mdx').read_text()
    counts = re.findall(r'^\| R[0-7].*\| (\d)/3 \|$', doc, re.M)
    assert len(counts) == 8 and sum(map(int, counts)) == 13
    assert here.name in doc
subprocess.run(['git', 'apply', '--reverse', '--check', str(here / 'source.patch')], cwd=root, check=True)
print('Verified exact source patch, original public contracts, initial build failures, ten-item scope, default60/60, isolated86/86 and13/24')
