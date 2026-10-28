import pathlib, shlex, subprocess
build=pathlib.Path('/workspace/zyx-build-r1-release')
root=pathlib.Path('/workspace/zyx')
lines=subprocess.check_output(['ninja','-t','commands','libzyx_core.a'],cwd=build,text=True).splitlines()
compile_line=next(x for x in lines if ' -c /workspace/zyx/src/query/execution/LogicalPlanReadRuntime.cpp' in x)
base=shlex.split(compile_line)
def compile_file(source, output, enabled):
    args=[]; i=0
    while i<len(base):
        a=base[i]
        if a in ('-o','-MT','-MF'): i+=2; continue
        if a in ('-c','-MD') or a.startswith('-DZYX_ENABLE_STORAGE_TEST_HOOKS=') or a.startswith('-DZYX_EXPERIMENTAL_BORROWED_GROUP_KEY=') or a.endswith('/LogicalPlanReadRuntime.cpp'): i+=1; continue
        args.append(a); i+=1
    args += [f'-DZYX_EXPERIMENTAL_BORROWED_GROUP_KEY={enabled}', '-c',str(source),'-o',str(output)]
    subprocess.run(args,cwd=build,check=True)
compile_file(root/'benchmarks/architecture-design/decisions/20261027-column-block/group-key-review/persistent-runner.cpp','/tmp/zyx-borrowed-runner.o',0)
link=shlex.split(subprocess.check_output(['ninja','-t','commands','zyx_test_suite'],cwd=build,text=True).splitlines()[-1])
link=[x for x in link if x not in (':','&&') and not x.endswith('.o')]
idx=link.index('-o')
for enabled in (0,1):
    compile_file(root/'src/query/execution/LogicalPlanReadRuntime.cpp',f'/tmp/zyx-borrowed-runtime-{enabled}.o',enabled)
    args=link.copy(); args[idx+1]=f'/tmp/zyx-borrowed-runner-{enabled}'
    # Objects must precede static archives for link-time symbol resolution.
    args[1:1]=[f'/tmp/zyx-borrowed-runtime-{enabled}.o','/tmp/zyx-borrowed-runner.o']
    subprocess.run(args,cwd=build,check=True)
