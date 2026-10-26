"""Separate Emscripten's filesystem owner from the async module bootstrap.

The FS object is copied verbatim, except its one dynamic heap read uses an
explicit getter. Each module invocation owns a distinct FS; no runtime state is
shared through the imported module. Fail closed when generator shape changes.
"""
from pathlib import Path
import argparse
import re

BINDINGS = ('ENVIRONMENT_IS_WORKER', 'FS_fileDataToTypedArray', 'FS_getMode',
            'FS_modeStringToFlags', 'MEMFS', 'Module', 'PATH', 'PATH_FS', 'TTY',
            'UTF8ArrayToString', 'abort', 'err', 'intArrayFromString', 'mmapAlloc',
            'out', 'randomFill', 'readBinary')


def split_runtime(source: Path):
    original = source.read_text()
    if 'createZyxFilesystem' in original:
        if not source.with_name('zyx-filesystem.mjs').is_file():
            raise ValueError('filesystem module missing from already split runtime')
        return
    # Preserve the script-tag factory contract across Emscripten wrapper versions.
    original = original.replace('var createZyxModule =', 'globalThis.createZyxModule =', 1)
    if 'globalThis.createZyxModule =' not in original:
        raise ValueError('generated module export changed; review bootstrap contract')
    start = original.index('  var FS = {\n')
    end = original.index('\n  };', start) + len('\n  };')
    body = original[start:end]
    if len(re.findall(r'\bHEAP8\b', body)) != 1 or 'writeChunks(stream, HEAP8,' not in body:
        raise ValueError('generated heap access changed; review filesystem binding')
    body = body.replace('writeChunks(stream, HEAP8,', 'writeChunks(stream, getHeap8(),')
    module = ('// Generated filesystem owner; regenerate with scripts/build_wasm.sh.\n'
              'export function createZyxFilesystem({\n  '+', '.join(BINDINGS)+', getHeap8\n}) {\n'+body+'\n  return FS;\n}\n')
    arguments = ', '.join(name if name != 'randomFill' else 'randomFill: (...args) => randomFill(...args)' for name in BINDINGS)
    replacement = '''  // ES module code is cached; filesystem state is private to this invocation.
  var filesystemPath = locateFile('zyx-filesystem.mjs');
  if (ENVIRONMENT_IS_NODE && !isFileURI(filesystemPath))
    filesystemPath = require('node:url').pathToFileURL(filesystemPath).href;
  var { createZyxFilesystem } = await import(filesystemPath);
  var FS = createZyxFilesystem({
    '''+arguments+''', getHeap8: () => HEAP8
  });'''
    loader = original[:start]+replacement+original[end:]
    for name, text in [('zyx.js', loader), ('zyx-filesystem.mjs', module)]:
        if len(text.splitlines()) > 3000:
            raise ValueError(f'{name} exceeds 3000 lines; review runtime responsibilities')
    source.with_name('zyx-filesystem.mjs').write_text(module)
    source.write_text(loader)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    split_runtime(parser.parse_args().source)
