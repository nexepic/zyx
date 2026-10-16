from pathlib import Path
import subprocess,shlex
root=Path('/workspace/zyx')
source=Path('/tmp/zyx-procedure-buffer-control.cpp')
source.write_bytes(subprocess.check_output(['git','show','ce2b467a:src/query/execution/LogicalPlanProcedureRuntime.cpp'],cwd=root))
command=shlex.split(Path('/tmp/zyx-procedure-baseline-compile.txt').read_text())
command=[x.replace('/tmp/zyx-procedure-baseline','/tmp/zyx-procedure-buffer-control') for x in command]
subprocess.run(command,cwd=root/'buildDir',check=True)
link=shlex.split((root/'buildDir/global-output-link-command.txt').read_text())
link.insert(1,'/tmp/zyx-procedure-buffer-control.o')
link[link.index('-o')+1]='/tmp/zyx-procedure-buffer-control-tests'
subprocess.run(link,cwd=root/'buildDir',check=True)
run=subprocess.run(['/tmp/zyx-procedure-buffer-control-tests','--gtest_filter=LogicalVectorPropertyBatchTest.ProcedureRowBackingRejectsQuotaBeforePropertyCopiesAndRefunds','--gtest_output=xml:/tmp/zyx-procedure-buffer-control.xml'],cwd=root)
assert run.returncode==1, 'negative control must expose the old unadmitted buffer'
