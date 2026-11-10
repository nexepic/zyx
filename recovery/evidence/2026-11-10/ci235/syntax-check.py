import json, shlex, subprocess
from pathlib import Path

root=Path('/workspace/zyx-ci-reconstructed')
out=Path('/workspace/zyx-ci235-syntax');out.mkdir(exist_ok=True)
commands=json.loads((root/'buildDir/compile_commands.json').read_text())
records=[]
for mode in ('no-hooks','asan-ubsan-hooks'):
    for source in ('LogicalPlanProcedureRuntime.cpp','LogicalPlanReadRuntime.cpp'):
        entry=next(e for e in commands if e['file'].endswith('/'+source))
        args=entry.get('arguments') or shlex.split(entry['command'])
        filtered=[];skip=False
        for arg in args:
            if skip:skip=False;continue
            if arg in ('-o','-MF','-MT','-MQ'):skip=True;continue
            if arg in ('-c','-MD','-MMD','-fprofile-instr-generate','-fcoverage-mapping'):continue
            if mode=='no-hooks' and arg.startswith('-DZYX_ENABLE_STORAGE_TEST_HOOKS'):continue
            filtered.append(arg)
        filtered+=['-fsyntax-only']
        if mode=='asan-ubsan-hooks':filtered+=['-fsanitize=address,undefined']
        p=subprocess.run(filtered,cwd=entry['directory'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
        (out/(mode+'-'+source+'.log')).write_bytes(p.stdout)
        record={'mode':mode,'source':source,'exit':p.returncode};records.append(record);print(record,flush=True)
        if p.returncode:raise SystemExit(p.returncode)
(out/'summary.json').write_text(json.dumps(records,indent=2)+'\n')
