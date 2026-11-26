"""Large canonical values spanning WAL windows. Every read opens a new process."""
import shutil
import signal
import subprocess


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
    run("large-row-read", scoped, size)  # Reopen the WAL and checkpoint on close.
    run("large-row-read", scoped, size)  # Independently audit the clean base.
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
