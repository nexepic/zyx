import sys
_original_excepthook = sys.excepthook
def _selected_excepthook(kind, value, traceback):
    if kind.__name__ == 'CalledProcessError':
        print('Selected verification failed, exit:', value.returncode, file=sys.stderr)
    else:
        _original_excepthook(kind, value, traceback)
sys.excepthook = _selected_excepthook
import pathlib, subprocess, shlex, os
root=pathlib.Path('/workspace/zyx')
sources=['tests/src/storage/relocatable/index/test_BTreePageStoreTransactionOverlayStress.cpp']
objects=['CMakeFiles/zyx_test_suite.dir/'+s+'.o' for s in sources]
subprocess.run(['cmake','--build','buildDir','--target','zyx_core',*objects,*(['zyx_transaction_resource_fault_worker'] if os.environ.get('ZYX_BUILD_RESOURCE_WORKER') else []),'-j','4'],cwd=root,check=True)
lines=(root/'buildDir/build.ninja').read_text().splitlines()
start=next(i for i,v in enumerate(lines) if v.startswith('build zyx_test_suite:'))
libs=next(v.split(' = ',1)[1] for v in lines[start:start+14] if v.strip().startswith('LINK_LIBRARIES = '))
command=['/workspace/zyx-sysroot/usr/bin/clang++-19','-fprofile-instr-generate','-fcoverage-mapping','-pthread',*[str(root/'buildDir'/o) for o in objects],'/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a',*shlex.split(libs),'-o','/tmp/zyx-dense-rebuild-transfer-tests']
(root/'buildDir/global-output-link-command.txt').write_text(shlex.join(command)+'\n')
subprocess.run(command,cwd=root/'buildDir',check=True)
subprocess.run(['/tmp/zyx-dense-rebuild-transfer-tests','--gtest_output=xml:/tmp/zyx-dense-rebuild-transfer-tests.xml','--gtest_filter='+os.environ.get('ZYX_TARGETED_GTEST_FILTER','*')],cwd=root,check=True)
