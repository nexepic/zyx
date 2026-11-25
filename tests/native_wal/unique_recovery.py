"""Public Cypher semantics, schema publication and WAL receipt failures."""
import hashlib
import os
import struct
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

driver, shim=map(os.path.abspath,sys.argv[1:3])
def run(*args,code=0,error=None):
    p=subprocess.run([driver,*map(str,args)],env={**os.environ,"LD_PRELOAD":shim},text=True,capture_output=True,timeout=30)
    assert p.returncode==code,(args,p.returncode,p.stdout,p.stderr)
    if error is not None:
        assert error in p.stderr, (args, p.stderr)
    return p.stdout.strip()

with tempfile.TemporaryDirectory() as d:
    root=Path(d)
    labels=Path(run("init",root/"labels"))
    run("labels",labels)
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
    run("overlay", clone(active, "overlay"))
    run("large-delta", clone(active, "large-delta"))
    run("owner-limit", clone(active, "owner-limit"))
    unindexed = Path(run("unindexed", root / "unindexed"))
    run("undeclared", unindexed)
    # Produce matching integer columns and postings with a duplicate using a
    # real unconstrained writer. Re-seal the same base with a declaration:
    # successful byte authentication must still fail its semantic audit.
    fabricated = Path(run("init", root / "duplicate-base"))
    run("duplicate-in-base", fabricated)
    image = bytearray(fabricated.read_bytes()[:-32])
    assert image[:8] == b"ZYXng004"
    pos = 112
    for expected in (b"Item", b"k"):
        size = struct.unpack_from("<Q", image, pos)[0]
        assert image[pos + 8:pos + 8 + size] == expected
        pos += 8 + size
    image[:8] = b"ZYXng005"
    image[pos:pos] = struct.pack("<Q", 1) + b"u" + image[24:32]
    fabricated.write_bytes(image + hashlib.sha256(image).digest())
    run("defined", fabricated, code=1, error="duplicate key")
    prefix = clone(active, "prefix")
    run("prefix", prefix)
    run("accept-prefix", clone(prefix, "legal-prefix"))
    truncated = clone(prefix, "truncated-prefix")
    with truncated.with_suffix(".wal").open("r+b") as f:
        f.truncate(os.fstat(f.fileno()).st_size - 1)
    run("accept-prefix", truncated, code=1, error="Missing native graph WAL prefix")
    # Keep both WAL transactions, use a duplicate in the first and leave the
    # later repair intact. The receipt authenticates these exact modified bytes.
    log = prefix.with_suffix(".wal")
    contents = log.read_bytes()
    marker = struct.pack("<Q", 4328719365)
    assert contents.count(marker) == 1
    invalid = contents.replace(marker, struct.pack("<Q", 1))
    log.write_bytes(invalid)
    receipt = bytearray(prefix.read_bytes())
    assert len(receipt) == 112 and receipt[48:80] == hashlib.sha256(contents).digest()
    receipt[48:80] = hashlib.sha256(invalid).digest()
    receipt[80:] = hashlib.sha256(receipt[:80]).digest()
    prefix.write_bytes(receipt)
    run("accept-prefix", prefix, code=1, error="duplicate key")
    # Copy only a closed, independently durable root. The writable copy can
    # advance and enforce its declaration without changing the source.
    run("defined", active)  # Close the writable source cleanly before copying.
    backup_dir = root / "writable-copy"
    backup_dir.mkdir()
    backup = backup_dir / "main"
    shutil.copyfile(active, backup)
    run("defined", backup)
    run("change", backup)
    run("changed", backup)
    run("defined", active)
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
