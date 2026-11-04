import xml.etree.ElementTree as E,subprocess,os
from pathlib import Path
r=E.parse('/tmp/zyx-cell-atomic-regression.xml').getroot()
names=list(dict.fromkeys(t.get('classname')+'.'+t.get('name') for t in r.iter('testcase') if not t.get('name').startswith('DISABLED_')))
names=[n.replace('UnmodifiedOverlayForwardsCellsAndChangedOverlayFallsBack','OverlayCellsSupportChangedViewsAndMergeStagedValues') for n in names]

names=list(dict.fromkeys(names+[t.get('classname')+'.'+t.get('name') for t in E.parse('/tmp/zyx-query-fence-focused.xml').getroot().iter('testcase')]))
env=os.environ.copy();env['LD_LIBRARY_PATH']='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'
for i in range(0,len(names),350):
 group=names[i:i+350]; prefix='/tmp/zyx-query-fence-regression-'+str(i//350)
 with open(prefix+'.log','w') as out:
  run=subprocess.run(['/workspace/zyx-ci-config/zyx_test_suite','--gtest_filter='+':'.join(group),'--gtest_output=xml:'+prefix+'.xml'],env=env,stdout=out,stderr=subprocess.STDOUT)
 print(i//350,len(group),run.returncode,flush=True)
print('selected',len(names),flush=True)
