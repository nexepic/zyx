"""Per-child process resources, without sampling query execution."""
import concurrent.futures,os,subprocess

def run(command,stdout,stderr,env,limits,timeout=1200):
    process=subprocess.Popen(command,stdout=stdout,stderr=stderr,env=env,preexec_fn=limits)
    timed_out=False
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
        pending=pool.submit(os.wait4,process.pid,0)
        try:
            _,status,usage=pending.result(timeout=timeout)
        except concurrent.futures.TimeoutError:
            timed_out=True;process.kill()
            _,status,usage=pending.result()
    process.returncode=os.waitstatus_to_exitcode(status)
    return {'returncode':process.returncode,'timed_out':timed_out,
            'process_peak_rss_kib':usage.ru_maxrss,'process_user_cpu_seconds':usage.ru_utime,
            'process_system_cpu_seconds':usage.ru_stime,'process_input_blocks':usage.ru_inblock,
            'process_output_blocks':usage.ru_oublock,
            'scope':'whole worker including open, private copy, queries and close; block counts are OS accounting, not attributed query I/O'}
