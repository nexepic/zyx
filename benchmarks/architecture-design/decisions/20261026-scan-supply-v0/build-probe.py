import pathlib,json,subprocess,shutil
r=pathlib.Path('/workspace/zyx');o=r/'benchmarks/architecture-design/decisions/20261026-scan-supply-v0';d=o
base=r/'benchmarks/architecture-design/decisions/20261022-core-cost-buffer/post-buffer-cpu'
# Diagnostic headers are versioned locally; do not overwrite them.
a=json.loads((r/'benchmarks/architecture-design/decisions/20261026-s3-production-lifecycle/public-b-recipes.json').read_text())['compile'];a[a.index('-c')+1]=str(d/'cpu-probe.cpp');a[a.index('-o')+1]=str(d/'probe.o')
for flag in ['-MF','-MT']:
 if flag in a:a[a.index(flag)+1]=str(d/('probe.d' if flag=='-MF' else 'probe.o'))
b=json.loads((r/'benchmarks/architecture-design/decisions/20261026-s4-native-write-cost/public-b-final-link.json').read_text());b=[str(d/'probe.o') if x.endswith('/public-b.o') else x for x in b];b[b.index('-o')+1]=str(d/'probe');b+=['-ldl','-lrt'];
with (o/'probe-build.log').open('w') as f:
 for c in [a,b]:subprocess.run(c,cwd=r,stdout=f,stderr=subprocess.STDOUT,check=True)
(o/'probe-recipes.json').write_text(json.dumps({'compile':a,'link':b},indent=2))
