import json,subprocess,hashlib
from pathlib import Path
r=Path('/workspace/zyx/benchmarks/architecture-design/results/20261020-work-resume');out=Path('/workspace/zyx-cost-diagnostic')
d=json.loads((r/'r7-final-public-probes-build.json').read_text())['after-row']
s=(r/'r7-final-public-probe.cpp').read_text()
s=s.replace('   const auto start=timing?Clock::now():Clock::time_point{};','   graph::debug::PerfTrace::reset(); graph::debug::PerfTrace::setEnabled(iteration>=0);\n   const auto start=timing?Clock::now():Clock::time_point{};',1)
needle='   ProbeRows persisted;std::string verify;'
s=s.replace(needle,'''   graph::debug::PerfTrace::setEnabled(false);
   if(iteration>=0){auto stats=graph::debug::PerfTrace::snapshotAndReset();for(const auto &[key,e]:stats)
    std::cerr<<name<<"\\t"<<key<<"\\t"<<e.totalNs<<"\\t"<<e.calls<<"\\t"<<e.totalValue<<"\\t"<<e.valueCalls<<"\\n";}
'''+needle,1)
(out/'probe.cpp').write_text(s)
def convert(args):
 result=[]
 for x in args:
  x=x.replace(str(r/'r7-final-public-probe.cpp'),str(out/'probe.cpp')).replace(str(r/'r7-final-public-probe-after-row.o'),str(out/'probe.o')).replace(str(r/'r7-final-public-probe-after-row.d'),str(out/'probe.d')).replace(str(r/'r7-final-public-probe-after-row'),str(out/'probe')).replace('--dependency-file=CMakeFiles/zyx_compare_bench.dir/link.d','--dependency-file='+str(out/'link.d'))
  result.append(x)
 return result
actual=hashlib.sha256((Path(d['cwd'])/'libzyx_core.a').read_bytes()).hexdigest()
print('engine sha',actual,'matches frozen',actual==d['engine_archive_sha256'],flush=True)
assert actual in (r/'r7-membership/build.json').read_text()
(out/'identity.json').write_text(json.dumps({'library_sha256':actual,'evidence':str(r/'r7-membership/build.json'),'diagnostic_only':True},indent=2))
with (out/'build.log').open('w') as log:
 for args in [d['compile'],d['link']]:subprocess.run(convert(args),cwd=d['cwd'],stdout=log,stderr=subprocess.STDOUT,check=True)
print('built')
