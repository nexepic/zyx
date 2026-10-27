from pathlib import Path
import json,subprocess
N=Path('/workspace/zyx/benchmarks/architecture-design/decisions/20261027-column-block'); old=N/'current-cpu'; new=N/'current-stacks'
assert not (new/'protocol.json').exists(), 'Do not overwrite measured artifacts'
new.mkdir(exist_ok=True)
for name in ['probe.cpp','cpu-phase.hpp','cpu-sampler.hpp']:
 (new/name).write_bytes((old/name).read_bytes())
p=new/'cpu-sampler.hpp';s=p.read_text().replace('#include <signal.h>','#include <signal.h>\n#include <unwind.h>')
s=s.replace('inline std::array<uintptr_t, 65536> pcs{};','inline std::array<uintptr_t, 65536> pcs{};\ninline std::array<std::array<uintptr_t,32>,65536> stacks{};\ninline std::array<unsigned,65536> ordinals{};\nstruct StackWriter { uintptr_t *frames; unsigned size=0; };\ninline _Unwind_Reason_Code unwindFrame(_Unwind_Context *context, void *argument) {\n auto &writer=*static_cast<StackWriter *>(argument);\n if(writer.size==32)return _URC_END_OF_STACK;\n writer.frames[writer.size++]=_Unwind_GetIP(context); return _URC_NO_REASON;\n}')
s=s.replace('weights[static_cast<size_t>(i)] =', 'StackWriter writer{stacks[static_cast<size_t>(i)].data()};\n        _Unwind_Backtrace(unwindFrame,&writer);\n        ordinals[static_cast<size_t>(i)]=calls;\n        weights[static_cast<size_t>(i)] =')
s=s.replace('    std::fclose(out);','''    std::fclose(out);
    const std::string stackPath=std::string(path)+".stacks";
    out=std::fopen(stackPath.c_str(),"w"); if(!out)return;
    for(sig_atomic_t i=0;i<count;++i)for(size_t j=0;j<32 && stacks[i][j];++j) {
      Dl_info info{};const auto pc=stacks[i][j];
      if(dladdr(reinterpret_cast<void *>(pc),&info) && info.dli_fname && info.dli_fbase)
        std::fprintf(out,"%d\\t%u\\t%zu\\t%s\\t%llx\\n",int(i),ordinals[i],j,info.dli_fname,
          static_cast<unsigned long long>(pc-reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    std::fclose(out);''')
s=s.replace('#include <stdexcept>','#include <stdexcept>\n#include <string>')
p.write_text(s)
# Warm the unwinder before measurement. Diagnostic only: unwinding is not a production signal handler.
p=new/'probe.cpp';s=p.read_text();s=s.replace('cpu_sample::begin();','cpu_sample::begin();');p.write_text(s)
for i in [0,1]:
 cmd=json.loads((old/f'command-{i}.json').read_text());cmd=[x.replace(str(old),str(new)) for x in cmd]
 (new/f'command-{i}.json').write_text(json.dumps(cmd,indent=2)+'\n')
 with (new/f'build-{i}.log').open('w') as out:r=subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError((new/f'build-{i}.log').read_text()[-3000:])
print('built stack diagnostic; unchanged engine')
