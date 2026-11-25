"""Exercise coherent projected checkpoints and their confirmed WAL suffix in fresh processes."""
from pathlib import Path
import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile

driver, shim = map(os.path.abspath,sys.argv[1:3])
env = {**os.environ, "LD_PRELOAD":shim}

def run(*args, expected=0):
    proc=subprocess.run([driver,*map(str,args)],env=env,text=True,capture_output=True,timeout=30)
    assert proc.returncode==expected,(args,proc.returncode,proc.stderr)
    return proc

with tempfile.TemporaryDirectory() as name:
    root=Path(name)
    seed=root/"seed"
    run("create",seed)
    run("seed",seed/"main")
    def fresh(name):
        return shutil.copytree(seed,root/name)/"main"
    resident=root/"resident"
    run("create",resident)
    run("resident-seed",resident/"main")
    shutil.copytree(resident,root/"deletions")
    run("resident-deletions",root/"deletions"/"main")
    assert run("resident-kill",resident/"main",expected=-signal.SIGKILL).stdout.strip()=="ack"
    shutil.copytree(resident,root/"budget")
    run("resident-budget",root/"budget"/"main")
    run("resident-replay",resident/"main")
    p=fresh("functional")
    run("change",p)
    run("verify",p)
    run("export",str(p)+".A")
    merged=fresh("merge")
    assert run("merge-crash",merged,expected=-signal.SIGKILL).stdout.strip()=="ack"
    run("merge-reopen",merged)
    run("merge-reopen",merged)
    run("mixed",fresh("mixed"))
    cases=0
    for op,objects in [("fault-commit","wm"),("fault-checkpoint","m")]:
        for obj in objects:
            for event,code in [("write=partial-eio",2),("write=short",0),("sync=eio",2),
                               ("synced=kill",-signal.SIGKILL),("renamed=kill",-signal.SIGKILL),
                               ("dirsync=eio",2),("dirsynced=kill",-signal.SIGKILL)]:
                path=fresh(str(cases))
                run(op,path,f"{obj}.{event}",expected=code)
                visible=(op=="fault-checkpoint" or code==0 or
                         (obj=="m" and event.split("=")[0] in ("renamed","dirsync","dirsynced")))
                run("new" if visible else "old",path)
                cases+=1
    # A successful control supplies the exact projection offsets. Each record
    # is sixteen bytes. The final digest is separate from the coalesced body.
    control=fresh("cut-control")
    run("fault-checkpoint",control,"trace")
    size=control.stat().st_size
    for lane,end_offset in [("column",32+32+8),("posting",32+8)]:
        path=fresh("cut-"+lane)
        run("fault-checkpoint",path,f"m.body-cut={size-end_offset}",expected=2)
        run("new",path)
    # A resealed inconsistent column or posting must fail the independent
    # row-coverage audit before exposing even a valid-looking empty result.
    for i,field in enumerate(["column","posting"]):
        path=fresh(field)
        image=bytearray(path.read_bytes())
        image[-32-(48 if i==0 else 16)] ^= 1
        image[-32:]=hashlib.sha256(image[:-32]).digest()
        path.write_bytes(image)
        assert "mismatch" in run("old",path,expected=1).stderr
    # The same revision and declaration in another database are insufficient.
    foreign=root/"foreign"
    run("create",foreign)
    run("seed",foreign/"main")
    run("fault-commit",foreign/"main","m.dirsynced=kill",expected=-signal.SIGKILL)
    for dependency in ["wal","base"]:
        path=fresh("cross-"+dependency)
        run("fault-commit",path,"m.dirsynced=kill",expected=-signal.SIGKILL)
        side=path.with_suffix("."+dependency)
        side.write_bytes((foreign/side.name).read_bytes())
        message=run("new",path,expected=1).stderr
        assert "mismatch" in message or "checksum" in message,message
    path=fresh("required")
    run("fault-commit",path,"m.dirsynced=kill",expected=-signal.SIGKILL)
    path.with_suffix(".wal").unlink()
    assert run("new",path,expected=1).stderr
    print(cases,"fault windows, recovery and projected coverage checks passed")
