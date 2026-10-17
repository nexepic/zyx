from pathlib import Path
import hashlib,json,shutil,tempfile

def digest(path):
    result=hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda:stream.read(1024*1024),b''):result.update(block)
    return result.hexdigest()

def install_temp_journal(root, case, module):
    original=module.TemporaryDirectory
    target=root/'owned-temporaries'/(case+'.json')
    target.parent.mkdir(parents=True,exist_ok=True)
    records=[]
    def create(*args,**kwargs):
        directory=original(*args,**kwargs)
        path=Path(directory.name)
        if path.name.startswith(('zyx-contract-','kuzu-contract-')):
            observed=path.stat()
            records.append({'path':str(path),'device':observed.st_dev,'inode':observed.st_ino})
            temporary=target.with_suffix('.json.tmp')
            temporary.write_text(json.dumps(records,indent=2)+'\n');temporary.replace(target)
        return directory
    module.TemporaryDirectory=create
    return original

def remove_owned(path):
    files=[path] if path.is_file() else [p for p in path.rglob('*') if p.is_file()]
    if path.is_symlink() or any(p.is_symlink() for p in files):raise RuntimeError('Refuse unexpected symlink in owned case artifact')
    observed=[{'path':str(p),'bytes':p.stat().st_size,'sha256':digest(p)} for p in files]
    if path.is_dir():shutil.rmtree(path)
    else:path.unlink()
    return observed

def cleanup_contract(root, case):
    ledger=root/'owned-temporaries'/(case+'.json')
    if not ledger.exists():return
    cleaned=[]
    for record in json.loads(ledger.read_text()):
        path=Path(record['path'])
        if not path.exists():continue
        observed=path.stat()
        if path.parent.resolve()!=Path(tempfile.gettempdir()).resolve() or not path.name.startswith(('zyx-contract-','kuzu-contract-')) or path.is_symlink() or (observed.st_dev,observed.st_ino)!=(record['device'],record['inode']):
            raise RuntimeError('Owned temporary identity changed; leave it untouched')
        cleaned.extend(remove_owned(path))
    (root/'owned-temporaries'/(case+'-cleanup.json')).write_text(json.dumps({'after_case_process_exit':True,'removed':cleaned},indent=2)+'\n')

def cleanup_read(root, scale, number, engine, profile):
    folder=root/'reads'/scale/f'round-{number}'/engine/profile
    cleaned=[]
    if folder.exists():
        for result in folder.iterdir():
            if not result.is_dir() or result.is_symlink():continue
            for path in result.iterdir():
                if path.name=='dataset' or path.name.startswith(('zyx.db','kuzu.db')):
                    cleaned.extend(remove_owned(path))
    target=root/'read-cleanups'/scale/profile/f'round-{number}'/(engine+'.json');target.parent.mkdir(parents=True,exist_ok=True)
    target.write_text(json.dumps({'after_case_process_exit':True,'removed':cleaned},indent=2)+'\n')
