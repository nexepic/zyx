import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from runner import workload_preflight as f


class ProtocolWorker:
    instances = []
    def __init__(self, *args):
        self.trace = []
        self.instances.append(self)
    def call(self, *fields, **kwargs):
        self.trace.append({'command': list(fields[:2]), 'expected': kwargs.get('expected'), 'sampling_phase': getattr(getattr(self, 'family_budget', None), 'sampling_phase', 'setup')})
        if fields[0] == 'STATS':
            return {'file_bytes': 1, 'rss_bytes': 1, 'read_bytes': 1, 'write_bytes': 1}
        return {'ok': True}
    def close(self): pass


class FamilyIsolationTest(unittest.TestCase):
    def test_native_rejects_unknown_family_before_binary_or_output_access(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / 'out'
            with self.assertRaisesRegex(ValueError, 'unsupported isolated'):
                f.run_native(Path(tmp) / 'missing-binary', out, family='unknown')
            self.assertFalse(out.exists())

    def test_kuzu_rejects_native_only_family_before_creating_output(self):
        with tempfile.TemporaryDirectory() as tmp, patch.dict(sys.modules, {'kuzu': SimpleNamespace()}):
            out = Path(tmp) / 'out'
            with self.assertRaisesRegex(ValueError, 'unsupported isolated'):
                f.run_kuzu(out, family='dynamic')
            self.assertFalse(out.exists())

    def run_protocol(self, family):
        with tempfile.TemporaryDirectory() as tmp, patch.object(f, 'NativeWorker', ProtocolWorker):
            binary = Path(tmp) / 'worker'; binary.write_bytes(b'protocol-only-not-executed')
            out = Path(tmp) / 'out'
            report = f.run_native(binary, out, profile='smoke', family=family)
            trace = json.loads((out / 'trace.json').read_text())
            self.assertEqual(report['families'], [family])
            self.assertEqual(report['isolated_family'], family)
            return report, trace

    def test_maintenance_keeps_ten_windows_without_other_family_setups(self):
        report, trace = self.run_protocol('maintenance')
        self.assertEqual(len(report['maintenance_windows']), 10)
        commands = [row['command'] for row in trace]
        self.assertEqual(sum(row[0] == 'MAINTAIN' for row in commands), 10)
        deletes = [row for row in trace if row['command'][:2] == ['QUERY', 'MATCH (n:Churn) DELETE n RETURN 1 AS deleted']]
        self.assertEqual(len(deletes), 10)
        self.assertTrue(all(row['expected'] == [[1]] for row in deletes))
        text = json.dumps(commands)
        for foreign in (':Aggregate', ':Dynamic', ':Wide', 'MIXED_THREADS'):
            self.assertNotIn(foreign, text)

    def test_snapshot_has_a_fresh_score_model_without_running_mixed(self):
        _, trace = self.run_protocol('snapshot')
        commands = [row['command'] for row in trace]
        self.assertFalse(any(row[0] == 'MIXED_THREADS' for row in commands))
        old_read = next(row for row in trace if row['command'][:2] == ['TXQUERY', 'old'])
        self.assertEqual(old_read['expected'], [[0]])

    def test_contract_maintenance_excludes_three_warmup_windows_from_ten_reported_windows(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(f, 'NativeWorker', ProtocolWorker):
            binary = Path(tmp) / 'worker'; binary.write_bytes(b'protocol-only-not-executed')
            report = f.run_native(binary, Path(tmp) / 'out', profile='contract', family='maintenance')
            trace = ProtocolWorker.instances[-1].trace
            writes = [r for r in trace if r['command'][0] == 'PARAM' and '(:Churn' in r['command'][1]]
            self.assertEqual(sum(r['sampling_phase'] == 'warmup' for r in writes), 300)
            self.assertEqual(sum(r['sampling_phase'] == 'measure' for r in writes), 1000)
            self.assertEqual(len(report['maintenance_windows']), 10)
            self.assertEqual(sum(r['command'][0] == 'MAINTAIN' for r in trace), 13)
