from pathlib import Path
import hashlib,json,os,shutil,subprocess,time
repo=Path('/workspace/zyx');out=repo/'benchmarks/architecture-design/results/20261022-core-cost/final';inputs=out/'inputs';inputs.mkdir(exist_ok=True)
probe=repo/'benchmarks/architecture-design/results/20261022-core-cost/free-summary-diagnostic/probe';archive=Path('/workspace/zyx-build-core-candidate/libzyx_core.a');sha=lambda p:hashlib.file_digest(p.open('rb'),'sha256').hexdigest()
for scale in ['medium','holdout']:
 source=repo/f'benchmarks/architecture-design/results/20261020-work-resume/r7-inputs/{scale}-r7-final-indexed.zyx';target=inputs/(scale+'-summary.zyx')
 if target.exists():raise RuntimeError('retain existing input and receipt')
 shutil.copyfile(source,target)
 with target.open('rb') as f:os.fsync(f.fileno())
 before=target.stat().st_size;start=time.monotonic()
 with (out/(scale+'-summary-init.stdout')).open('w') as stdout,(out/(scale+'-summary-init.stderr')).open('w') as stderr:
  p=subprocess.Popen([str(probe),'initialize-free-summary',str(target)],stdout=stdout,stderr=stderr,env=dict(os.environ,LD_LIBRARY_PATH='/workspace/zyx-sysroot/usr/lib/x86_64-linux-gnu'))
  pid,status,usage=os.wait4(p.pid,0);p.returncode=os.waitstatus_to_exitcode(status)
 record={'purpose':'explicit legacy input initialization; excluded from ordinary writes, reported separately','returncode':p.returncode,'source':str(source),'target':str(target),'wall_seconds':time.monotonic()-start,'before_bytes':before,'after_bytes':target.stat().st_size,'user_seconds':usage.ru_utime,'system_seconds':usage.ru_stime,'peak_rss_bytes':usage.ru_maxrss*1024,'os_read_bytes':usage.ru_inblock*512,'os_write_bytes':usage.ru_oublock*512,'source_sha256':sha(source),'target_sha256':sha(target),'library_sha256':sha(archive),'probe_sha256':sha(probe)}
 (out/(scale+'-summary-init.json')).write_text(json.dumps(record,indent=2)+'\n');print(scale,p.returncode,record['wall_seconds'],flush=True)
 if p.returncode:raise SystemExit(p.returncode)
