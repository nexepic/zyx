"""Reject missing, skipped, failed and duplicated soak contracts."""
import importlib.util
import pathlib
import tempfile
import unittest
import xml.etree.ElementTree as ET

spec = importlib.util.spec_from_file_location(
    'resource_soak', pathlib.Path(__file__).with_name('test_resource_recovery_soak.py'))
soak = importlib.util.module_from_spec(spec)
spec.loader.exec_module(soak)


class ResourceSoakEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = pathlib.Path(self.directory.name) / 'tests.xml'
        self.root = ET.Element('testsuites')
        for name in soak.CASES:
            suite, case = name.split('.', 1)
            ET.SubElement(self.root, 'testcase', classname=suite, name=case,
                          status='run', result='completed')

    def verify(self):
        ET.ElementTree(self.root).write(self.path)
        return soak.verify_selected_xml(self.path)

    def test_complete_success_is_accepted(self):
        self.assertEqual(self.verify(), len(soak.CASES))

    def test_missing_contract_is_rejected(self):
        self.root.remove(self.root[0])
        with self.assertRaises(RuntimeError):
            self.verify()

    def test_unexpected_contract_is_rejected(self):
        self.root[0].set('name', 'DifferentContract')
        with self.assertRaises(RuntimeError):
            self.verify()

    def test_duplicate_cannot_hide_a_failed_contract(self):
        failed = ET.SubElement(self.root, 'testcase', **self.root[0].attrib)
        ET.SubElement(failed, 'failure')
        with self.assertRaises(RuntimeError):
            self.verify()

    def test_skipped_and_failed_contracts_are_rejected(self):
        for tag in ('skipped', 'failure'):
            with self.subTest(tag=tag):
                child = ET.SubElement(self.root[0], tag)
                with self.assertRaises(RuntimeError):
                    self.verify()
                self.root[0].remove(child)

    def test_notrun_and_suppressed_contracts_are_rejected(self):
        for attribute, value in (('status', 'notrun'), ('result', 'suppressed')):
            with self.subTest(attribute=attribute):
                original = self.root[0].attrib[attribute]
                self.root[0].set(attribute, value)
                with self.assertRaises(RuntimeError):
                    self.verify()
                self.root[0].set(attribute, original)


class ResourceProcessSoakEvidenceTest(unittest.TestCase):
    def setUp(self):
        import test_resource_recovery_process_soak
        self.process = test_resource_recovery_process_soak
        self.lines = [prefix + name + ' (0 ms)'
                      for _ in range(2) for name in soak.CASES
                      for prefix in ('[ RUN      ] ', '[       OK ] ')]

    def test_all_rounds_must_pass(self):
        self.process.verify_repetitions('\n'.join(self.lines), 2)

    def test_missing_success_cannot_hide_behind_final_xml(self):
        del self.lines[1]
        with self.assertRaises(RuntimeError):
            self.process.verify_repetitions('\n'.join(self.lines), 2)

    def test_duplicate_execution_is_rejected(self):
        self.lines += self.lines[:2]
        with self.assertRaises(RuntimeError):
            self.process.verify_repetitions('\n'.join(self.lines), 2)

    def test_unknown_case_cannot_replace_a_required_case(self):
        self.lines[0] = '[ RUN      ] Other.Case'
        with self.assertRaises(RuntimeError):
            self.process.verify_repetitions('\n'.join(self.lines), 2)


if __name__ == '__main__':
    unittest.main()
