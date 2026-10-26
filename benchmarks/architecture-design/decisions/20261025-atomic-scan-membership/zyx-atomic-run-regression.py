import json,subprocess,os
recipes=json.load(open('/tmp/zyx-atomic-membership-regression-compile.json'))
base=json.load(open('/tmp/zyx-label-fallback-link.json'))
links={}
for profile,rs in recipes.items():
 objs=[r[r.index('-o')+1] for r in rs]
 assert all(os.path.exists(p) for p in objs),objs
 build={'isolated':'build-scan-candidate','default':'build-scan-work','compat':'build-hybrid-compat'}[profile]
 tail=base[base.index('/workspace/zyx/build-scan-candidate/libzyx_core.a')+1:]
 out='/tmp/zyx-atomic-'+profile+'-tests'
 cmd=[base[0]]+objs+['/workspace/zyx/'+build+'/libzyx_core.a']+tail
 cmd[cmd.index('-o')+1]=out
 links[profile]=cmd
 with open(out+'-link.log','w') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
 with open(out+'.log','w') as f:subprocess.run([out,'--gtest_output=json:'+out+'.json'],stdout=f,stderr=subprocess.STDOUT,check=True)
 print(profile,json.load(open(out+'.json'))['tests'],flush=True)
json.dump(links,open('/tmp/zyx-atomic-membership-regression-link.json','w'),indent=2)
