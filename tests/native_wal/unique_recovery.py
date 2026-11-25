"""Public Cypher semantics, schema publication and WAL receipt failures."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

driver, shim=map(os.path.abspath,sys.argv[1:3])
def run(*args,code=0):
    p=subprocess.run([driver,*map(str,args)],env={**os.environ,"LD_PRELOAD":shim},text=True,capture_output=True,timeout=30)
    assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
    return p.stdout.strip()

with tempfile.TemporaryDirectory() as d:
    root=Path(d)
    seed=Path(run("init",root/"seed"));run("seed",seed)
    def clone(source,name):return shutil.copytree(source.parent,root/name)/"main"
    test=clone(seed,"semantics");assert run("semantics",test,code=-signal.SIGKILL)=="ack";run("replay",test)
    dirty=Path(run("init",root/"dirty"));run("dirty-seed",dirty)
    events=[("write=partial-eio",2),("write=short",0),("sync=eio",2),("synced=kill",-signal.SIGKILL),
            ("renamed=kill",-signal.SIGKILL),("dirsync=eio",2),("dirsynced=kill",-signal.SIGKILL)]
    for source in [seed,dirty]:
        for event,code in events:
            p=clone(source,source.parent.name+event);run("ddl",p,"m."+event,code=code)
            visible=code==0 or event.split("=")[0] in ("renamed","dirsync","dirsynced")
            run("defined" if visible else "old",p)
    cancel=clone(seed,"cancelled");run("cancel",cancel);run("defined",cancel)
    active=clone(seed,"unique");run("ddl",active)
    for obj in ("w","m"):
        for event,code in events:
            p=clone(active,obj+event);run("change",p,obj+"."+event,code=code)
            visible=code==0 or (obj=="m" and event.split("=")[0] in ("renamed","dirsync","dirsynced"))
            run("changed" if visible else "defined",p)
    confirmed=clone(active,"confirmed");run("change",confirmed)
    for event,code in events:
        p=clone(confirmed,"checkpoint"+event);run("checkpoint",p,"m."+event,code=code);run("changed",p)
    miss=clone(confirmed,"missing");miss.with_suffix(".wal").unlink();run("changed",miss,code=1)
    foreign=Path(run("init",root/"foreign"));run("seed",foreign);run("ddl",foreign);run("change",foreign)
    for ext in (".wal",".base"):
        p=clone(confirmed,"wrong"+ext);shutil.copyfile(foreign.with_suffix(ext),p.with_suffix(ext));run("changed",p,code=1)
    print("Public UNIQUE, snapshot fencing, cancellation, mixed key changes, NULL/missing, WAL replay, foreign/missing dependencies and 35 fault cases passed")
