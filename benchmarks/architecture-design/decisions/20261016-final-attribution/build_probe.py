from pathlib import Path
import shlex,subprocess,json,hashlib
root=Path('/workspace/zyx');source=root/'benchmarks/architecture-design/decisions/20261016-final-attribution/mechanism-probe.cpp';obj=Path('/tmp/zyx-mechanism-probe.o');binary=Path('/tmp/zyx-mechanism-probe')
base=shlex.split(Path('/tmp/zyx-bounded-compare-compile-command.txt').read_text().strip().splitlines()[-1]);cmd=[];i=0
while i<len(base):
 if base[i] in ['-o','-MF','-MT','-c']:i+=2;continue
 if base[i]=='-MD':i+=1;continue
 cmd.append(base[i]);i+=1
cmd+=['-c',str(source),'-o',str(obj)];subprocess.run(cmd,cwd=root/'buildDir_perf_control',check=True)
lines=(root/'buildDir_perf_control/build.ninja').read_text().splitlines();start=next(i for i,v in enumerate(lines) if v.startswith('build zyx-compare-bench:'));libs=next(v.split(' = ',1)[1] for v in lines[start:start+15] if v.strip().startswith('LINK_LIBRARIES = '));link=[cmd[0],'-O3','-DNDEBUG',str(obj),*shlex.split(libs),'-o',str(binary)];subprocess.run(link,cwd=root/'buildDir_perf_control',check=True)
(root/'benchmarks/architecture-design/decisions/20261016-final-attribution/build-provenance.json').write_text(json.dumps({'compile':cmd,'link':link,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest()},indent=2)+'\n')
