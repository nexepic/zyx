from pathlib import Path
import json,subprocess,hashlib,shutil
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design/results/20261022-core-cost';dest=root/'post-buffer-cpu';dest.mkdir(exist_ok=True)
(dest/'cpu-phase.hpp').write_text('''#pragma once
#include <cstdint>
namespace cpu_phase {
inline thread_local uint8_t current = 0;
struct Scope {
 bool root;
 explicit Scope(uint8_t id) noexcept : root(current == 0) { if(root) current=id; }
 ~Scope() { if(root) current=0; }
};
}
''')
s=(repo/'benchmarks/architecture-design/decisions/20261022-core-cost-read-cpu/cpu-sampler.hpp').read_text();s='#include "cpu-phase.hpp"\n'+s
s=s.replace('inline std::array<uint32_t, 65536> weights{};','inline std::array<uint32_t, 65536> weights{};\ninline std::array<uint8_t,65536> phases{};')
s=s.replace('''        count = i + 1;''','''        phases[static_cast<size_t>(i)] = cpu_phase::current;
        count = i + 1;''')
s=s.replace('"%s\\t%llx\\t%u\\n"','"%s\\t%llx\\t%u\\t%u\\n"').replace('weights[static_cast<size_t>(i)]);','weights[static_cast<size_t>(i)], unsigned(phases[static_cast<size_t>(i)]));')
s=s.replace('"unknown\\t%llx\\t%u\\n"','"unknown\\t%llx\\t%u\\t%u\\n"');(dest/'cpu-sampler.hpp').write_text(s)
recipe=json.loads((repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/build.json').read_text())['public']
base=[a.replace('/workspace/zyx-build-r7-row','/workspace/zyx-build-core-candidate') for a in recipe['compile']]
commands=[];objects=[];sources={
 'src/storage/relocatable/property/PropertyCollectionRootStore.cpp':('PropertyCollectionRootScanCursor::readEntityPropertiesOptionalSorted(',1),
 'src/storage/relocatable/entity/PageBackedGraphReadAdapter.cpp':('PageBackedGraphReadAdapter::readNodeLabelMatchesSorted(',2),
 'src/storage/relocatable/index/BTreeIndex.cpp':('BTreeIndex::nextRangeCursor(',3)}
def compile_source(path,name):
 cmd=base[:]
 for flag in ['-o','-MT','-MF']:cmd[cmd.index(flag)+1]=str(dest/(name+('.d' if flag=='-MF' else '.o')))
 cmd[-1]=str(path);commands.append(cmd);objects.append(str(dest/(name+'.o')))
for source,(function,phase) in sources.items():
 s=(repo/source).read_text();start=s.index(function);brace=s.index('{',start);s=s[:brace+1]+f'\n        cpu_phase::Scope cpuPhase({phase});'+s[brace+1:];s='#include "cpu-phase.hpp"\n'+s
 path=dest/Path(source).name;path.write_text(s);compile_source(path,path.stem)
s=(repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/public-probe.cpp').read_text();s='#include "cpu-sampler.hpp"\n'+s;s=s.replace('   const auto start=timing?Clock::now():Clock::time_point{};','   cpu_sample::begin();\n   const auto start=timing?Clock::now():Clock::time_point{};',1);s=s.replace('   const double ms=timing?std::chrono::duration<double,std::milli>(Clock::now()-start).count():0;','   const double ms=timing?std::chrono::duration<double,std::milli>(Clock::now()-start).count():0;\n   cpu_sample::end();',1);probe=dest/'cpu-probe.cpp';probe.write_text(s);compile_source(probe,'cpu-probe')
link=[str(dest/'cpu-probe.o') if a.endswith('/public.o') else str(dest/'cpu-probe') if a.endswith('/public-baseline') else '--dependency-file='+str(dest/'cpu-link.d') if a.startswith('--dependency-file=') else a for a in recipe['link']];link[link.index('libzyx_core.a'):link.index('libzyx_core.a')]=objects[:-1];link+=['-ldl','-lrt']
with (dest/'build.log').open('w') as log:
 for cmd in commands+[link]:subprocess.run(cmd,cwd='/workspace/zyx-build-core-candidate',stdout=log,stderr=subprocess.STDOUT,check=True)
(dest/'build.json').write_text(json.dumps({'compile':commands,'link':link,'cwd':'/workspace/zyx-build-core-candidate','purpose':'diagnostic root-stage CPU attribution, tagged shadow translation units; production source unchanged','phases':{'0':'remaining executor/binding/other','1':'property batch provider including direct column delivery','2':'entity label validation batch','3':'BTree range entry production'},'library_sha256':hashlib.file_digest(Path('/workspace/zyx-build-core-candidate/libzyx_core.a').open('rb'),'sha256').hexdigest()},indent=2)+'\n');print('phase observer built')
