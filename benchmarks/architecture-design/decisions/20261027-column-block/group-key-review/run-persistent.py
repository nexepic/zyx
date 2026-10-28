import hashlib, json, os, pathlib, subprocess, sys
here=pathlib.Path(__file__).resolve().parent
bins=[pathlib.Path('/tmp/zyx-borrowed-runner-0'), pathlib.Path('/tmp/zyx-borrowed-runner-1')]
for binary in bins: assert binary.is_file(), binary
receipt={'protocol_sha256':hashlib.sha256((here/'persistent-protocol.json').read_bytes()).hexdigest(),
         'binaries':{str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in bins}, 'runs':[]}
fixtures=json.loads((here/'fixture-receipt.json').read_text())['fixtures']
if len(fixtures)!=10 or any(x['returncode'] for x in fixtures): raise SystemExit('incomplete fixture matrix')
for fixture in fixtures:
    file=pathlib.Path(fixture['path'])
    if hashlib.sha256(file.read_bytes()).hexdigest()!=fixture['sha256']: raise SystemExit('fixture hash changed')
    for process in range(3):
        children=[]; errors=[]
        try:
            for arm,binary in enumerate(bins):
                err=(here/f"stderr-{fixture['rows']}-{fixture['mode']}-{process}-{arm}.log").open('w');errors.append(err)
                children.append(subprocess.Popen([str(binary),str(file),str(fixture['rows']),str(fixture['mode'])],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=err,text=True,bufsize=1))
            samples=[[],[]]
            for iteration in range(11):
                for arm in ((iteration+process)&1, 1-((iteration+process)&1)):
                    child=children[arm]; child.stdin.write('run\n');child.stdin.flush();line=child.stdout.readline().strip()
                    if not line.startswith(f'sample={iteration} '): raise RuntimeError(f'unexpected arm {arm} result: {line}')
                    samples[arm].append(line)
            for child in children: child.stdin.close()
            for arm,child in enumerate(children):
                _,status,usage=os.wait4(child.pid,0);child.returncode=os.waitstatus_to_exitcode(status)
                item={'rows':fixture['rows'],'mode':fixture['mode'],'process':process,'arm':arm,'exit':child.returncode,
                      'rss_kib_process':usage.ru_maxrss,'user_seconds':usage.ru_utime,'system_seconds':usage.ru_stime,
                      'samples':samples[arm]}
                receipt['runs'].append(item)
                (here/'persistent-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
                if child.returncode: raise RuntimeError(f'arm {arm} exit {child.returncode}')
        except BaseException:
            for child in children:
                if child.poll() is None: child.kill();child.wait()
            raise
        finally:
            for err in errors: err.close()
