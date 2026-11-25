"""Run against the native SDK driver and Linux syscall interposer, in fresh directories."""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

driver, shim = map(os.path.abspath, sys.argv[1:3])
env = {**os.environ, "LD_PRELOAD": shim}
def run(op, path, fault=None, code=0, error=None):
    args = [driver, op, str(path)] + ([fault] if fault else [])
    proc = subprocess.run(args, env=env, capture_output=True, text=True, timeout=25)
    assert proc.returncode == code, (args, proc.returncode, proc.stderr)
    if error is not None:
        assert error in proc.stderr, (args, proc.stderr, error)
    return proc.stdout.strip()

def recovered(path, expected):
    assert run("count", path) == str(expected), (path, expected)
    assert not path.with_suffix(".wal").exists()

with tempfile.TemporaryDirectory() as name:
    root = Path(name)
    def new(label):
        return Path(run("init", root / label))
    graph = new("graph")
    run("graph", graph)
    run("exported",graph.with_suffix(".cp003"))
    run("cycles",new("cycles"))
    run("rollover",new("rollover"))
    # These events only occur when a *new* WAL is being installed. Arm
    # after the first generation exists, then check the last actual ack.
    for event, code in (("w.renamed=kill", -signal.SIGKILL), ("w.sync=eio", 2)):
        path = new("rollover-" + event)
        acknowledged = run("rollover",path,event,code).splitlines()
        assert 1 < len(acknowledged) < 26, acknowledged
        run("rollover-value",path,acknowledged[-1])
        assert not path.with_suffix(".wal").exists()
        print("automatic rollover",event,"restored",len(acknowledged),"acknowledged updates")
    run("types",new("types"))
    run("topology",new("topology"))
    run("cap",new("cap"))
    run("budget",new("budget"))
    run("uncertain",new("uncertain"))
    print("independent export reopen; 12 checkpoints/24 commits with pinned reader: pass")
    print("public Cypher graphs, private writes, stable IDs, snapshot, checkpoint, conflict, rollback, export: pass")
    killed = -signal.SIGKILL
    count = 0
    for kind in ("w", "m"):
        for stage in ("write", "sync", "synced", "rename", "renamed", "dirsync", "dirsynced"):
            event = kind+"."+stage
            path = new(event)
            run("append", path, event+"=kill", killed)
            # Linux process kill retains the kernel namespace, including a
            # rename before the directory fence. No caller acknowledgment yet.
            recovered(path, int(kind=="m" and stage in ("renamed","dirsync","dirsynced")))
            count += 1
    for kind in ("w", "m"):
        for stage in ("write","sync","rename","dirsync"):
            event = kind+"."+stage
            path = new(event+"-eio")
            run("append",path,event+"=eio",2)
            recovered(path,int(kind=="m" and stage=="dirsync"))
        path = new(kind+"-zero")
        run("append",path,kind+".write=zero",2)
        recovered(path,0)
        path = new(kind+"-short")
        run("append",path,kind+".write=short")
        recovered(path,1)
    original = new("confirmed")
    empty_main = original.read_bytes()
    assert run("leave",original,code=killed)=="ack"
    log = original.with_suffix(".wal")
    old_log = log.read_bytes()
    for event in ("m.write","m.synced","m.renamed","m.dirsynced","w.unlink","w.unlinked","u.dirsynced"):
        dest = root / ("checkpoint-"+event)
        shutil.copytree(original.parent,dest)
        run("checkpoint",dest/"main",event+"=kill",killed)
        recovered(dest/"main",1)
        count += 1
    outsider = new("outsider")
    run("leave",outsider,code=killed)
    for label, value in (("missing",None),("truncated",old_log[:-1]),("different",outsider.with_suffix(".wal").read_bytes())):
        dest=root/label
        shutil.copytree(original.parent,dest)
        p=dest/"main.wal"
        if value is None:p.unlink()
        else:p.write_bytes(value)
        run("count",dest/"main",code=2,error={"missing":"requires missing WAL","truncated":"Missing native graph WAL prefix","different":"WAL receipt mismatch"}[label])
    # A matching outer digest must not mask a stale generation or a gap
    # within the supposedly committed prefix.
    for label, offset, error in (("identity",8,"WAL generation mismatch"),("sequence",old_log.index(b"ZYXtx002")+8,"WAL sequence gap")):
        dest=root/("resigned-"+label)
        shutil.copytree(original.parent,dest)
        changed=bytearray(old_log)
        changed[offset] ^= 8
        receipt=bytearray(original.read_bytes())
        receipt[48:80]=hashlib.sha256(changed).digest()
        receipt[-32:]=hashlib.sha256(receipt[:-32]).digest()
        (dest/"main.wal").write_bytes(changed)
        (dest/"main").write_bytes(receipt)
        run("count",dest/"main",code=2,error=error)
    suffix=root/"unconfirmed-suffix"
    shutil.copytree(original.parent,suffix)
    with (suffix/"main.wal").open("ab") as out:out.write(b"incomplete following transaction")
    recovered(suffix/"main",1)
    recovered(original,1)
    log.write_bytes(outsider.with_suffix(".wal").read_bytes())
    recovered(original,1)
    lock_path = new("locks")
    with subprocess.Popen([driver,"hold",str(lock_path)],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True) as holder:
        assert holder.stdout.readline().strip()=="locked"
        run("append",lock_path,code=2)
        holder.communicate("\n",timeout=10)
    recovered(lock_path,0)
    with subprocess.Popen([driver,"hold-read",str(lock_path)],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True) as reader:
        assert reader.stdout.readline().strip()=="shared"
        run("read-only",lock_path)
        run("append",lock_path,code=2)
        reader.communicate("\n",timeout=10)
    alias = root/"alias"
    alias.mkdir()
    os.link(lock_path,alias/"main")
    run("append",alias/"main",code=2)
    run("append",lock_path,code=2)
    (alias/"main").unlink()
    recovered(lock_path,0)
    print(f"{count} pre-ack/checkpoint SIGKILL windows, post-ack replay, short/zero write, sync/rename EIO, required/stale WAL, concurrent open, last close: pass")

    # Model stable file contents separately from directory entries. Feed each
    # possible old/new namespace back to the real native recovery path. Verify
    # the fences that constrain these combinations from actual intercepted I/O.
    pair = new("chain")
    child = subprocess.run([driver,"chain",str(pair),"trace"],env=env,capture_output=True,text=True,timeout=25)
    assert child.returncode==killed and child.stdout.strip()=="ack", child
    stages=[line[3:] for line in child.stderr.splitlines() if line.startswith("IO ")]
    expected=["a.write","a.sync","a.synced"]+[kind+"."+event for kind in ("m",) for event in
              ("write","sync","synced","rename","renamed","dirsync","dirsynced")]
    assert stages==expected,(stages,expected)
    for event in ("a.write","a.sync","a.synced","m.write","m.renamed","m.dirsynced"):
        damaged=new("hot-"+event)
        run("chain",damaged,event+"=kill",killed)
        recovered(damaged,2 if event in ("m.renamed","m.dirsynced") else 1)
    for stage in ("a.write","a.sync","m.sync","m.dirsync"):
        damaged=new("hot-err-"+stage)
        run("chain",damaged,stage+"=eio",2)
        recovered(damaged,2 if stage=="m.dirsync" else 1)
    partial=new("partial-append")
    run("chain",partial,"a.write=partial-eio",2)
    recovered(partial,1)
    for stage in ("m.sync","m.renamed","m.dirsynced","w.unlinked","u.dirsynced"):
        dest=root/("checkpoint-after-appends"+stage)
        shutil.copytree(pair.parent,dest)
        run("checkpoint",dest/"main",stage+"=kill",killed)
        recovered(dest/"main",2)
    complete=new("hot-short")
    run("chain",complete,"a.write=short",killed)
    recovered(complete,2)
    def model(label,main,wal,expected):
        directory=root/("model-"+label)
        directory.mkdir()
        target=directory/"main"
        target.write_bytes(main)
        if wal is not None:(directory/"main.wal").write_bytes(wal)
        recovered(target,expected)
    prior=pair.with_suffix(".prior").read_bytes()
    old=pair.with_suffix(".prior-wal").read_bytes()
    new_log=pair.with_suffix(".wal").read_bytes()
    new_main=pair.read_bytes()
    assert new_log.startswith(old) and len(new_log)>len(old)
    model("first-log-bytes-name-lost",empty_main,None,0)
    model("first-log-name-durable",empty_main,old_log,0)
    model("unpublished-wal-name",prior,old,1)
    model("replacement-log-old-receipt",prior,new_log,1)
    model("completed-receipt",new_main,new_log,2)
    damaged = root/"old-generation"
    shutil.copytree(pair.parent,damaged)
    (damaged/"main.wal").write_bytes(old)
    run("count",damaged/"main",code=2,error="Missing native graph WAL prefix")
    # Losing the main-directory sync can leave a readable receipt. Recovery
    # must retain its log if even the fresh standalone rewrite cannot sync.
    retry = root/"failed-rewrite"
    shutil.copytree(pair.parent,retry)
    for _ in range(2):
        run("checkpoint",retry/"main","m.sync=eio",code=2)
        assert (retry/"main.wal").read_bytes()==new_log
        assert (retry/"main").read_bytes()==new_main
    recovered(retry/"main",2)
    run("checkpoint",pair)
    model("standalone-resurrected-name",pair.read_bytes(),old,2)
    model("standalone-clean-name",pair.read_bytes(),None,2)
    print("durable namespace model: 7 replay states; real commit order:"," ".join(stages))
