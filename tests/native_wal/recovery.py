"""Run against the native SDK driver and Linux syscall interposer, in fresh directories."""
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

driver, shim = map(os.path.abspath, sys.argv[1:3])
env = {**os.environ, "LD_PRELOAD": shim}
def run(op, path, fault=None, code=0, error=None, timeout=25):
    args = [driver, op, str(path)] + ([fault] if fault else [])
    proc = subprocess.run(args, env=env, capture_output=True, text=True, timeout=timeout)
    assert proc.returncode == code, (args, proc.returncode, proc.stderr)
    if error is not None:
        assert error in proc.stderr, (args, proc.stderr, error)
    return proc.stdout.strip()

def recovered(path, expected):
    assert run("count", path) == str(expected), (path, expected)
    assert not path.with_suffix(".wal").exists()
    assert not path.with_suffix(".base").exists()

with tempfile.TemporaryDirectory() as name:
    root = Path(name)
    def new(label):
        return Path(run("init", root / label))
    tiny = new("tiny-wal")
    run("tiny-wal-writes", tiny, code=-signal.SIGKILL)
    recovered(tiny, 2)
    for prefix, count in (("w", 0), ("a", 1)):
        tiny_fault = new("tiny-" + prefix)
        run("tiny-wal-writes", tiny_fault, prefix + ".write=before-padding-eio", code=2)
        recovered(tiny_fault, count)
    print("4 KiB version budget: aligned generation and append replayed; EIO before padding kept the prior receipt")
    run("long-limits", new("long-limits"))
    long_seed = new("long-seed")
    run("long-seed", long_seed)
    def long_copy(name):
        path = root / name / "main"
        path.parent.mkdir()
        shutil.copyfile(long_seed, path)
        return path
    independent = long_copy("long-life")
    run("long-life", independent)
    run("long-A", str(independent) + ".view")
    run("long-B", independent)
    run("long-delete", independent)
    run("long-A", long_seed)
    for tag, mode in (("w", "long-fault"), ("a", "long-append-fault")):
        for event, code, state in (("write=after-window-eio", 2, "A"),
                                   ("sync=eio", 2, "A"),
                                   ("write=short", -signal.SIGKILL, "B")):
            path = long_copy(tag + event)
            run(mode, path, tag + "." + event, code=code)
            run("long-" + state, path)
    dirty = long_copy("long-confirmed")
    run("long-fault", dirty, "m.renamed=kill", code=-signal.SIGKILL)
    for kind in ("truncated", "changed"):
        broken = root / ("long-" + kind) / "main"
        broken.parent.mkdir()
        for suffix in ("", ".wal", ".base"):
            shutil.copyfile(str(dirty) + suffix, str(broken) + suffix)
        wal = broken.with_suffix(".wal")
        data = bytearray(wal.read_bytes())
        assert len(data) > 3 * 32768
        if kind == "truncated":
            del data[2 * 32768:]
        else:
            data[2 * 32768 + 7] ^= 1
        wal.write_bytes(data)
        run("long-B", broken, code=2)
    run("long-B", dirty)
    print("Long graph: rollback, independent backup, pinned A/export A, checkpoint B, delete, 2 x 3 multi-window faults, 2 damaged prefixes and aggregate limit passed")
    budget = new("nested-budget")
    for op in ("nested-budget-write", "nested-budget-open"):
        before = hashlib.sha256(budget.read_bytes()).digest()
        run(op, budget, "32768", code=2, error="budget exceeded")
        assert before == hashlib.sha256(budget.read_bytes()).digest()
        run(op, budget, "2097152")
    recovered(budget, 1)
    run("nested-unique", new("nested-unique"))
    nested = new("nested")
    run("nested-write", nested, code=-signal.SIGKILL)
    # Preserve a valid digest/receipt while damaging the list count.
    # Structural rejection must happen before the decoder trusts that count.
    damaged = root / "damaged-nested" / "main"
    damaged.parent.mkdir()
    for suffix in ("", ".wal", ".base"):
        shutil.copyfile(str(nested) + suffix, str(damaged) + suffix)
    wal = damaged.with_suffix(".wal")
    wire = wal.read_bytes()
    marker = b"items" + bytes.fromhex("06 00 22 00 00 00 03 00 00 00")
    assert wire.count(marker) == 1
    invalid = wire.replace(marker, marker[:-4] + b"\xff" * 4)
    receipt = bytearray(damaged.read_bytes())
    assert receipt[48:80] == hashlib.sha256(wire).digest()
    receipt[48:80] = hashlib.sha256(invalid).digest()
    receipt[80:] = hashlib.sha256(receipt[:80]).digest()
    wal.write_bytes(invalid)
    damaged.write_bytes(receipt)
    run("nested-check", damaged, code=2)
    assert wal.read_bytes() == invalid and damaged.read_bytes() == receipt
    run("nested-check", nested)
    run("nested-check", str(nested) + ".snapshot")
    independent = root / "writable-copy"
    shutil.copyfile(nested, independent)
    run("nested-check", independent)
    run("leave", independent, code=-signal.SIGKILL)
    recovered(independent, 3)
    recovered(nested, 2)
    unreachable=new("unreachable-base")
    run("unreachable-base",unreachable)
    recovered(unreachable,1)
    replaced = new("replace-path")
    run("replace-path",replaced)
    recovered(replaced,0)
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
    run("locator-budget", new("locator-budget"))
    moved = new("relocation-graph")
    run("relocation-graph", moved, timeout=180)
    run("relocation-export", moved.with_suffix(".relocation.cp003"))
    print("Variable-width graph, independent export, two pinned versions and locator allocation denial: pass")
    print("Later staged row committed across physical relocation; 0->7->0 rejected with stale snapshot conflict")
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
    from lock_isolation import exercise_isolation
    exercise_isolation(root, new, run, recovered, driver, env)
    for event in ("b.link","b.linked","b.dirsync","b.dirsynced"):
        path=new("anchor-"+event)
        run("append",path,event+"=kill",killed)
        recovered(path,0)
    for event in ("b.link","b.dirsync"):
        path=new("anchor-error-"+event)
        run("append",path,event+"=eio",2)
        recovered(path,0)
    original = new("confirmed")
    empty_main = original.read_bytes()
    first=subprocess.run([driver,"leave",str(original),"trace"],env=env,capture_output=True,text=True,timeout=25)
    assert first.returncode==killed and first.stdout.strip()=="ack", first
    trace=[line[3:] for line in first.stderr.splitlines() if line.startswith("IO ")]
    required=["b.link","b.linked","b.dirsync","b.dirsynced"]+[kind+"."+stage for kind in ("w","m") for stage in
              ("write","sync","synced","rename","renamed","dirsync","dirsynced")]
    assert trace==required,trace
    print("first receipt fence order:", " ".join(trace))
    log = original.with_suffix(".wal")
    old_log = log.read_bytes()
    anchor_path=original.with_suffix(".base")
    anchor_bytes=anchor_path.read_bytes()
    run("append",anchor_path,code=2,error="Managed native dependency")
    run("read-only",anchor_path,code=2,error="Managed native dependency")
    assert anchor_path.read_bytes()==anchor_bytes and log.read_bytes()==old_log
    for event in ("m.write","m.synced","m.renamed","m.dirsynced","w.unlink","w.unlinked","u.dirsynced"):
        dest = root / ("checkpoint-"+event)
        shutil.copytree(original.parent,dest)
        run("checkpoint",dest/"main",event+"=kill",killed)
        recovered(dest/"main",1)
        count += 1
    for event in ("b.unlink","b.unlinked","d.dirsynced"):
        dest=root/("drop-anchor-"+event)
        shutil.copytree(original.parent,dest)
        run("checkpoint",dest/"main",event+"=kill",killed)
        recovered(dest/"main",1)
    outsider = new("outsider")
    run("leave",outsider,code=killed)
    for label, value in (("missing",None),("truncated",old_log[:-1]),("different",outsider.with_suffix(".wal").read_bytes())):
        dest=root/label
        shutil.copytree(original.parent,dest)
        p=dest/"main.wal"
        if value is None:p.unlink()
        else:p.write_bytes(value)
        run("count",dest/"main",code=2,error={"missing":"requires missing WAL","truncated":"Missing native graph WAL prefix","different":"WAL receipt mismatch"}[label])
    for label, blob, error in (("lost",None,"requires missing base"),("wrong",outsider.with_suffix(".base").read_bytes(),"base generation mismatch")):
        dest=root/("base-"+label)
        shutil.copytree(original.parent,dest)
        anchor_path=dest/"main.base"
        if blob is None:anchor_path.unlink()
        else:anchor_path.write_bytes(blob)
        run("count",dest/"main",code=2,error=error)
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
        if main in (prior,new_main):
            (directory/"main.base").write_bytes(anchor)
        if wal is not None:(directory/"main.wal").write_bytes(wal)
        recovered(target,expected)
    anchor=pair.with_suffix(".base").read_bytes()
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

    large=new("large")
    run("large-leave",large,code=killed)
    base=large.with_suffix(".base")
    wal=large.with_suffix(".wal")
    sizes=(base.stat().st_size,wal.stat().st_size)
    assert min(sizes)>512*1024 and sizes[0]>2*1024*1024
    before=[hashlib.sha256(p.read_bytes()).digest() for p in (large,base,wal)]
    upper=512*1024
    run("budget-point",large,str(upper))
    lower=0
    while lower+1<upper:
        probe=(upper+lower)//2
        child=subprocess.run([driver,"budget-point",str(large),str(probe)],env=env,capture_output=True,text=True,timeout=25)
        if child.returncode==0:upper=probe
        else:
            assert child.returncode==2 and "budget" in child.stderr,child
            lower=probe
    run("budget-point",large,str(upper))
    run("budget-point",large,str(lower),2,"budget")
    assert before==[hashlib.sha256(p.read_bytes()).digest() for p in (large,base,wal)]
    damaged=root/"large-corrupt"
    shutil.copytree(large.parent,damaged)
    run("late-damage",damaged/"main",str(512*1024))
    run("budget-point",damaged/"main",str(512*1024),2,"checksum mismatch")
    relocated = root/"checkpoint-locators"
    shutil.copytree(large.parent, relocated)
    run("checkpoint-locators", relocated/"main")
    print("Checkpoint locators: old pin retains one inode; released pin leaves none; shifted row survives reopen")
    run("budget-recover",large,str(512*1024))
    assert not base.exists() and not wal.exists()
    print("bounded recovery: base bytes",sizes[0],"WAL bytes",sizes[1],"first accepted budget",upper,"rejected",lower)
