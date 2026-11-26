"""Large canonical values spanning WAL windows. Every read opens a new process."""
import hashlib
from integrity_recovery import probe
import shutil
import signal
import subprocess


def audit_nested_string(driver, main, env, active_wal):
    payload = main.with_suffix(".wal") if active_wal else main
    with probe(driver, main, "ro", env) as verify:
        healthy = verify()
        assert healthy.startswith("ok "), healthy
        valid = payload.read_bytes()
        broken = bytearray(valid)
        data = valid.index(b"a" * 1024)
        assert valid[data - 10:data - 8] == bytes([5, 0])
        broken[data - 9] = 1  # Invalid flags on the nested canonical string.
        if active_wal:
            receipt = main.read_bytes()
            control = bytearray(receipt)
            assert control[48:80] == hashlib.sha256(valid).digest()
            control[48:80] = hashlib.sha256(broken).digest()
            control[-32:] = hashlib.sha256(control[:-32]).digest()
            main.write_bytes(control)
        else:
            assert valid.startswith(b"ZYXng")
            broken[-32:] = hashlib.sha256(broken[:-32]).digest()
        payload.write_bytes(broken)
        error = verify()
        assert error.startswith("invalid ") and "flags" in error, error
        payload.write_bytes(valid)
        if active_wal:
            main.write_bytes(receipt)
        assert verify() == healthy


def exercise(root, new, driver, env):
    def run(op, path, size, fault=None, expected=0):
        args = [driver, op, str(path), str(size)]
        if fault:
            args.append(fault)
        result = subprocess.run(args, env=env, capture_output=True, text=True, timeout=120)
        assert result.returncode == expected, (args, expected, result.returncode, result.stderr)

    killed = -signal.SIGKILL
    scoped = new("large-string-key")
    size = 16 << 20
    run("large-row-schema", scoped, size)
    run("large-row-write", scoped, size, expected=killed)
    audit_nested_string(driver, scoped, env, True)
    run("large-row-read", scoped, size)  # Reopen the WAL and checkpoint on close.
    run("large-row-read", scoped, size)  # Independently audit the clean base.
    audit_nested_string(driver, scoped, env, False)
    run("large-row-duplicate", scoped, size)

    for mode, tag, size in (("large-row-write", "w", 16 << 20), ("large-row-append", "a", 8 << 20)):
        path = new("large-row-" + tag)
        run(mode, path, size, expected=killed)
        wal = path.with_suffix(".wal").read_bytes()
        start = 0 if tag == "w" else wal.rfind(b"ZYXtx002")
        assert start >= 0 and len(wal) - start > size
        windows = (len(wal) - start + 32767) // 32768
        run("large-row-read", path, size)
        run("large-row-life", path, size)
        assert not path.with_suffix(".wal").exists()
        backup = root / ("backup-" + tag) / "main"
        backup.parent.mkdir()
        shutil.copyfile(path, backup)
        run("large-row-delete", backup, size)
        run("large-row-empty", backup, size)
        run("large-row-updated", path, size)

        for index, (event, code, check) in enumerate([
            *((f"{tag}.window={i}", 2, "large-row-empty") for i in (1, windows // 2, windows)),
            (tag + ".sync=eio", 2, "large-row-empty"), ("m.rename=kill", killed, "large-row-empty"),
            ("m.renamed=kill", killed, "large-row-read")
        ]):
            broken = new("large-row-" + tag + str(index))
            run(mode, broken, size, event, code)
            run(check, broken, size)
        print("verified large graph, old reader, rollback, writable backup and WAL faults", tag, size, windows)
