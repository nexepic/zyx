from pathlib import Path
import subprocess,shlex,json,re,sys
root=Path('/workspace/zyx'); build=Path(sys.argv[1]); out=Path(sys.argv[2]);out.mkdir(parents=True,exist_ok=True)
modules=['tests/src/storage/relocatable/property/test_HybridPropertyConversionPreparation.cpp','tests/src/storage/relocatable/property/test_HybridPropertyGroupConversion.cpp', 'tests/src/storage/relocatable/property/test_PropertySubblockGroupConversion.cpp', 'tests/src/storage/relocatable/property/test_HybridPropertyExtension.cpp', 'tests/src/storage/relocatable/property/test_HybridPropertyPageImage.cpp']
subprocess.run(['ninja','-C',str(build),'-j','2','libzyx_core.a'],check=True)
commands=subprocess.check_output(['ninja','-C',str(build),'-t','commands','zyx_test_suite'],text=True).splitlines();objects=[]
for m in modules:
 args=shlex.split(next(v for v in commands if ' -c '+str(root/m) in v));objects.append(args[args.index('-o')+1])
subprocess.run(['ninja','-C',str(build),'-j','2',*objects],check=True)
link=shlex.split(commands[-1].removeprefix(': && ').removesuffix(' && :'));link=[v for v in link if not v.endswith('.cpp.o')];link[link.index('-o'):link.index('-o')]=objects;binary=out/'records-test';link[link.index('-o')+1]=str(binary);link.append('/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu/libgtest_main.a');subprocess.run(link,cwd=build,check=True)
suites=sorted({s for m in modules for s in re.findall(r'\bTEST(?:_F)?\(\s*(\w+)\s*,',(root/m).read_text())}); filt=':'.join(s+'.*' for s in suites)
(out/'scope.json').write_text(json.dumps({'modules':modules,'filter':filt,'full_suite':False,'link':link},indent=2)+'\n')
subprocess.run([str(binary),'--gtest_filter='+filt,'--gtest_output=json:'+str(out/'results.json')],cwd=out,check=True)
