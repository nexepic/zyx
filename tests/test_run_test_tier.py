"""Contract tests for the explicit unit/stress partition."""
import importlib.util
from pathlib import Path


spec = importlib.util.spec_from_file_location(
    'run_test_tier', Path(__file__).resolve().parents[1] / 'scripts' / 'run_test_tier.py'
)
tier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tier)


def test_tier_filters_form_complementary_exact_patterns():
    names = tier.STRESS_TESTS
    assert len(names) == 4
    assert len(set(names)) == len(names)
    assert all('*' not in name and '?' not in name and ':' not in name for name in names)
    assert tier.tier_filter('stress') == ':'.join(names)
    assert tier.tier_filter('unit') == '*-' + tier.tier_filter('stress')


def test_main_propagates_failure_and_uses_existing_shard_launcher(monkeypatch, tmp_path):
    binary = tmp_path / 'suite'
    calls = []
    monkeypatch.setattr(tier.sys, 'argv', ['runner', 'stress', '--binary', str(binary), '--jobs', '1'])
    monkeypatch.setattr(tier.subprocess, 'call', lambda args, **kwargs: calls.append(args) or 17)
    assert tier.main() == 17
    assert Path(calls[0][1]).name == 'run_test_shards.py'
    assert calls[0][-1] == '--gtest_filter=' + tier.tier_filter('stress')


def test_xml_output_is_separate_for_single_and_parallel_runs(monkeypatch, tmp_path):
    binary = tmp_path / 'suite'
    calls = []
    monkeypatch.setattr(tier.subprocess, 'call', lambda args, **kwargs: calls.append(kwargs['env']) or 0)
    for jobs in ((1,) if tier.os.name == 'nt' else (1, 2)):
        monkeypatch.setattr(tier.sys, 'argv', ['runner', 'unit', '--binary', str(binary), '--jobs', str(jobs), '--xml-dir', str(tmp_path / str(jobs))])
        assert tier.main() == 0
    assert calls[0]['GTEST_OUTPUT'] == f"xml:{tmp_path / '1' / 'shard-0.xml'}"
    if tier.os.name != 'nt':
        assert calls[1]['ZYX_TEST_XML_DIR'] == str(tmp_path / '2')
