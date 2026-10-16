from pathlib import Path
import json,shlex,subprocess
r=Path('/workspace/zyx');b=r/'buildDir_perf_control';s=(r/'benchmarks/graphdb-comparison/workload-contract/native-worker.cpp').read_text();s=s.replace('#include <cmath>','#include <cmath>\n#include "graph/query/execution/PrepaidMemoryResource.hpp"',1)
n='\t\t\t\t} else if (command == "FORMAT") {'
x='''                    const auto usage = backend->queryResourceDomain()->usage();
                    response.pop_back();
                    for (size_t i = 0U; i < usage.classes.size(); ++i)
                        response += ",\\\"charged_class_" + std::to_string(i) + "\\\":" + std::to_string(usage.classes[i]);
                    if (auto cache = std::dynamic_pointer_cast<execution::detail::PrepaidMemoryResource>(backend->logicalPlanCacheMemoryOwner()))
                        response += ",\\\"cache_grant_used\\\":" + std::to_string(cache->used());
                    response += '}';
'''+n
assert n in s;s=s.replace(n,x,1)
s=s.replace('if (argc != 2 || std::filesystem::exists(argv[1]))', 'if (argc != 2)')
s=s.replace('auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();', 'const bool existing = std::filesystem::exists(argv[1]); if (existing) options.open_mode = OpenMode::OPEN_EXISTING_FILE; auto backend = existing ? std::make_shared<RelocatableStorageBackend>(options) : RelocatableStorageBackend::initializeNew(options); backend->open();')
p=Path('/tmp/zyx-resource-diagnostic-worker.cpp');p.write_text(s)
base=shlex.split(Path('/tmp/zyx-bounded-compare-compile-command.txt').read_text().strip().splitlines()[-1]);cmd=[];i=0
while i<len(base):
 if base[i] in ['-o','-MF','-MT','-c']:i+=2;continue
 if base[i]=='-MD':i+=1;continue
 cmd.append(base[i]);i+=1
obj=Path('/tmp/zyx-resource-diagnostic-worker.o');cmd+=['-c',str(p),'-o',str(obj)];subprocess.run(cmd,cwd=b,check=True)
lines=(b/'build.ninja').read_text().splitlines();start=next(i for i,v in enumerate(lines) if v.startswith('build zyx-workload-contract-worker:'));libs=next(v.split(' = ',1)[1] for v in lines[start:start+15] if v.strip().startswith('LINK_LIBRARIES = '));subprocess.run([cmd[0],'-O3','-DNDEBUG',str(obj),*shlex.split(libs),'-o','/tmp/zyx-resource-diagnostic-worker'],cwd=b,check=True)
