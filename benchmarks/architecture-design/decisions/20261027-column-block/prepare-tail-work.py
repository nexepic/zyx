from pathlib import Path
import json,subprocess
N=Path('/workspace/zyx/benchmarks/architecture-design/decisions/20261027-column-block'); old=N/'current-cpu';new=N/'tail-work';new.mkdir(exist_ok=False)
for name in ['probe.cpp','cpu-phase.hpp','cpu-sampler.hpp']:(new/name).write_bytes((old/name).read_bytes())
p=new/'probe.cpp';s=p.read_text();s=s.replace('   cpu_sample::begin();','''   struct Counts { std::array<long long, 9> values{}; } counts;
   auto receive = [](void *ptr, std::string_view key, int64_t value, bool) noexcept {
    constexpr std::array<std::string_view,9> keys={"logical_plan.direct_property.rows","native_graph.hybrid.group_decodes","native_graph.hybrid.column_membership_decodes","native_graph.hybrid.column_decodes","storage_io.file_size_calls","storage.buffer_pool.hits","storage.buffer_pool.misses","storage.checksum.bytes","logical_plan.aggregate.direct_input_rows"};
    for(size_t i=0;i<keys.size();++i)if(key==keys[i]){static_cast<Counts*>(ptr)->values[i]+=value;break;}
   };
   graph::debug::PerfTrace::ScopedObserver observer(&counts,receive);
   cpu_sample::begin();''').replace('   cpu_sample::end();','''   cpu_sample::end();
   if(iteration>=0) { std::cerr<<"{\\\"iteration\\\":"<<iteration<<",\\\"counts\\\":[";
    for(size_t i=0;i<counts.values.size();++i)std::cerr<<(i?",":"")<<counts.values[i];std::cerr<<"]}\\n"; }''')
p.write_text(s)
for i in (0,1):
 cmd=[x.replace(str(old),str(new)) for x in json.loads((old/f'command-{i}.json').read_text())];(new/f'command-{i}.json').write_text(json.dumps(cmd,indent=2)+'\n')
 with (new/f'build-{i}.log').open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 if r.returncode:raise RuntimeError((new/f'build-{i}.log').read_text()[-5000:])
print('built')
