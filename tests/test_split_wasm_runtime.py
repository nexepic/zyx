"""Generator-shape regressions for optional Emscripten filesystem helpers."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("split_wasm_runtime", Path(__file__).resolve().parents[1] / "scripts/split_wasm_runtime.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class SplitWasmRuntimeTest(unittest.TestCase):
    def test_optional_conversion_helper_follows_generated_fs(self):
        for helper_used in (True, False):
            with self.subTest(helper_used=helper_used), tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "zyx.js"
                helper = '  var FS_fileDataToTypedArray = data => data;\n' if helper_used else ''
                operation = 'FS_fileDataToTypedArray(data)' if helper_used else 'new Uint8Array(data)'
                source.write_text('var createZyxModule = async function() {\n' + helper +
                    '  var FS = {\n    convert(data) { return ' + operation + '; },\n' +
                    '    write(stream) { return writeChunks(stream, HEAP8, 0); }\n  };\n};\n')
                module.split_runtime(source)
                loader = source.read_text()
                owner = source.with_name('zyx-filesystem.mjs').read_text()
                self.assertEqual('FS_fileDataToTypedArray' in loader, helper_used)
                self.assertEqual('FS_fileDataToTypedArray' in owner, helper_used)
                self.assertIn('writeChunks(stream, getHeap8(),', owner)
                module.split_runtime(source)
                self.assertEqual(source.read_text(), loader)
                self.assertEqual(source.with_name('zyx-filesystem.mjs').read_text(), owner)

if __name__ == '__main__': unittest.main()
