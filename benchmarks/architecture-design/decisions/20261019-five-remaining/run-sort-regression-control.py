"""Reverse-check only the Sort quota regression using its parent runtime source.
Run the main necessary-module runner first. Arguments: BUILD_DIR RESULT_DIR OUTPUT_DIR.
This is a component correctness control, never a before performance binary.
"""
from pathlib import Path
import hashlib
import json
import shlex
import subprocess
import sys
root=Path(__file__).resolve().parents[4]
b=Path(sys.argv[1]).resolve();current=Path(sys.argv[2]).resolve();out=Path(sys.argv[3]).resolve()
out.mkdir(parents=True,exist_ok=True)
parent=json.loads((Path(__file__).parent/'registration.json').read_text())['parent']
source=root/'src/query/execution/LogicalPlanReadRuntime.cpp'
old=out/source.name;old.write_bytes(subprocess.check_output(['git','show',parent+':src/query/execution/LogicalPlanReadRuntime.cpp'],cwd=root))
commands=subprocess.check_output(['ninja','-C',str(b),'-t','commands','libzyx_core.a'],text=True).splitlines()
args=shlex.split(next(s for s in commands if ' -c '+str(source) in s))
obj=out/'old-runtime.o';args[args.index('-o')+1]=str(obj);args[args.index('-c')+1]=str(old)
args.extend(['-I',str(source.parent)]);subprocess.run(args,cwd=b,check=True)
link=json.loads((current/'test-scope.json').read_text())['link']
link[link.index('-o'):link.index('-o')]=[str(obj)];link[link.index('-o')+1]=str(out/'reverse-tests')
subprocess.run(link,cwd=b,check=True)
result=subprocess.run([str(out/'reverse-tests'),'--gtest_filter=LogicalPlanSortGrowthTest.SortKeyQuotaFailureIsTypedAndASeparateLargerExecutionCanRetry','--gtest_output=json:'+str(out/'results.json')],cwd=out)
report=json.loads((out/'results.json').read_text())
if result.returncode!=1 or report['tests']!=1 or report['failures']!=1:
    raise RuntimeError('Expected parent-source quota regression did not reproduce')
(out/'process.json').write_text(json.dumps({'source_parent':parent,'source_sha256':hashlib.sha256(old.read_bytes()).hexdigest(),'exit_code':result.returncode,'expected':'failure before typed-key-slot fix','compiler':args,'link':link},indent=2)+'\n')
