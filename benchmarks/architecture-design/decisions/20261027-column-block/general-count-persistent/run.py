#!/usr/bin/env python3
import hashlib, json, os, pathlib, subprocess, sys
here = pathlib.Path(__file__).resolve().parent
binary = pathlib.Path('/tmp/zyx-r1-release-tests')
assert binary.is_file()
metadata = {'protocol_sha256': hashlib.sha256((here/'protocol.json').read_bytes()).hexdigest(),
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'runs': []}
for process in range(3):
    for rows in (128, 512):
        for mixed in (0, 1):
            env = os.environ.copy(); env['ZYX_PROBE_ROWS'] = str(rows); env['ZYX_PROBE_MIXED'] = str(mixed)
            output = here / f'run-{process}-{rows}-{mixed}.log'
            with output.open('w') as out:
                child = subprocess.Popen([str(binary), '--gtest_also_run_disabled_tests',
                    '--gtest_filter=ScanReadProofTest.DISABLED_PersistentCountFusionProbe'], stdout=out, stderr=subprocess.STDOUT, env=env)
                _, status, usage = os.wait4(child.pid, 0); child.returncode = os.waitstatus_to_exitcode(status)
            metadata['runs'].append({'process':process, 'rows':rows, 'mixed':mixed,
                'exit_code':child.returncode, 'max_rss_kib_linux':usage.ru_maxrss,
                'user_cpu_seconds': usage.ru_utime, 'system_cpu_seconds': usage.ru_stime, 'file':output.name})
            (here/'receipt.json').write_text(json.dumps(metadata,indent=2)+'\n')
            if child.returncode:
                sys.exit(child.returncode)
