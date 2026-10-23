from pathlib import Path
import json,subprocess,hashlib
repo=Path('/workspace/zyx');root=repo/'benchmarks/architecture-design/results/20261022-core-cost';dest=root/'read-runs-mechanism';dest.mkdir(exist_ok=True)
base=(repo/'benchmarks/architecture-design/decisions/20261022-core-cost-p0/public-probe.cpp').read_text().replace('int main(int argc,char **argv)', 'int publicProbeMain(int argc,char **argv)');source=dest/'probe.cpp';source.write_text(base+'''

struct Mechanism {
 struct Entry { std::string_view key; int64_t sum=0, maximum=0; uint64_t calls=0; };
 std::array<Entry,8> entries{{
  {"logical_plan.direct_property.run_batches"},{"native_graph.read.property_text_runs"},
  {"native_graph.read.property_scalar_runs"},{"native_graph.read.property_run_cells"},
  {"native_graph.read.property_typed_text_cells"},{"native_graph.read.property_selected_borrowed_owners"},
  {"native_graph.read.property_selected_slot_copy_bytes"},{"native_graph.read.property_selected_column_decode"}}};
 static void receive(void *owner,std::string_view key,int64_t value,bool duration) noexcept {
  if(duration)return;
  for(auto &entry:static_cast<Mechanism *>(owner)->entries)if(entry.key==key){entry.sum+=value;++entry.calls;entry.maximum=std::max(entry.maximum,value);break;}
 }
};
int main(int argc,char **argv){
 Mechanism counts;int result;
 {graph::debug::PerfTrace::ScopedObserver observation(&counts,Mechanism::receive);result=publicProbeMain(argc,argv);}
 if(const auto *path=std::getenv("ZYX_MECHANISM_OUTPUT")){std::ofstream out(path);for(const auto &entry:counts.entries)out<<entry.key<<'\\t'<<entry.sum<<'\\t'<<entry.calls<<'\\t'<<entry.maximum<<'\\n';}
 return result;
}
''')
b=json.loads((root/'read-runs-targeted/build.json').read_text())['public'];cmd=b['compile'][:]
for f in ['-o','-MT','-MF']:cmd[cmd.index(f)+1]=str(dest/('probe.d' if f=='-MF' else 'probe.o'))
cmd[-1]=str(source)
link=[str(dest/'probe.o') if a==str(root/'read-runs-targeted/public.o') else a.replace('--dependency-file='+str(root/'read-runs-targeted/public-link.d'),'--dependency-file='+str(dest/'link.d')) for a in b['link']];link[link.index('-o')+1]=str(dest/'probe')
with (dest/'build.log').open('w') as log:
 for c in [cmd,link]:subprocess.run(c,cwd=b['cwd'],stdout=log,stderr=subprocess.STDOUT,check=True)
(dest/'build.json').write_text(json.dumps({'compile':cmd,'link':link,'cwd':b['cwd'],'library_sha256':hashlib.file_digest(Path('/workspace/zyx-build-core-candidate/libzyx_core.a').open('rb'),'sha256').hexdigest(),'timing':False,'purpose':'bounded fixed-key synchronous counter observer, actual P0 query and output oracle'},indent=2)+'\n')
