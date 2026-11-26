"""Check the persisted generation after opening a real storage backend."""
import hashlib
import shutil
from pathlib import Path
from subprocess import PIPE, Popen, run as run_process
from contextlib import contextmanager


def image(directory):
    return {p.name: (p.stat().st_ino, p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).digest())
            for p in directory.iterdir() if p.is_file()}


@contextmanager
def probe(driver, path, mode, env):
    proc = Popen([driver, "integrity-" + mode, str(path)], env=env, text=True,
                 stdin=PIPE, stdout=PIPE)
    try:
        assert proc.stdout.readline().strip() == "ready"
        def verify(command="audit"):
            before = image(path.parent)
            proc.stdin.write(command + "\n")
            proc.stdin.flush()
            result = proc.stdout.readline().strip()
            if command in ("audit", "exhaust"):
                assert image(path.parent) == before, "verification changed persistent state"
            return result
        yield verify
    finally:
        proc.stdin.close()
        assert proc.wait(timeout=30) == 0, (str(path), mode, proc.returncode)


def exercise(root, new, run, driver, env):
    main = new("integrity")
    for mode in ("ro", "rw"):
        with probe(driver, main, mode, env) as verify:
            assert verify().split()[1:8] == ["0"] * 7
            assert verify("exhaust") == "admitted after release"
    run("embedded-batch", main, "512")
    run("embedded-batch", main, "1")
    with probe(driver, main, "rw", env) as verify:
        control = verify()
        assert control.startswith("ok ") and control.split()[6] == "513", control
        original = main.read_bytes()
        slot = int.from_bytes(original[4112:4120], "little")
        for offset in (4096 * (2 + slot) + 40, len(original) - 1):
            broken = bytearray(original)
            broken[offset] ^= 1
            main.write_bytes(broken)
            error = verify()
            assert error.startswith("invalid "), error
            main.write_bytes(original)
            assert verify() == control
    run("seal-prefix", main)
    with probe(driver, main, "ro", env) as verify:
        control = verify()
        assert control.split()[4] == "1" and control.split()[6] == "514", control
        for suffix in (".wal", ".base"):
            dependency = main.with_suffix(suffix)
            saved = dependency.with_suffix(".displaced")
            dependency.rename(saved)
            error = verify()
            assert error.startswith("invalid ") and "missing" in error, error
            saved.rename(dependency)
            assert verify() == control
    competing = root / "audit-interleave"
    shutil.copytree(main.parent, competing)
    with Popen([driver, "integrity-rw", str(competing / "main")], env=env, text=True,
               stdin=PIPE, stdout=PIPE) as worker:
        assert worker.stdout.readline().strip() == "ready"
        worker.stdin.write("interleave\n")
        worker.stdin.flush()
        assert worker.stdout.readline().strip() == "paused"
        try:
            before = image(competing)
            for mode in ("ro", "rw"):
                rival = run_process([driver, "integrity-" + mode, str(competing / "main")],
                                    env=env, input="", capture_output=True, text=True, timeout=10)
                assert rival.returncode == 2 and "lock native graph main" in rival.stderr, rival
            assert image(competing) == before
        finally:
            worker.stdin.write("release\n")
            worker.stdin.flush()
        assert worker.stdout.readline().strip() == "serialized"
        worker.stdin.close()
        assert worker.wait(timeout=30) == 0
    # The intermediate seal root may be torn while the prior ordinary WAL
    # receipt remains authoritative. The verifier must not checkpoint it.
    interrupted = root / "audit-before-B"
    shutil.copytree(main.parent, interrupted)
    run("seal-batch", interrupted / "main", "s.root=partial-eio", 2)
    with probe(driver, interrupted / "main", "ro", env) as verify:
        assert verify().split()[6] == "514"
    run("seal-batch", main)
    duplicate = root / "integrity-standalone"
    duplicate.mkdir()
    copy = duplicate / "main"
    copy.write_bytes(main.read_bytes())
    for tested in (copy, main):
        with probe(driver, tested, "ro", env) as verify:
            result = verify().split()
            assert result[0] == "ok" and result[6] == "515" and result[4] == "0", result
    # A standalone, acknowledged B remains the last generation when C
    # only partially writes the inactive root slot.
    interrupted = root / "audit-after-B"
    shutil.copytree(main.parent, interrupted)
    earlier = main.read_bytes()
    run("embedded-batch", interrupted / "main", "1")
    later = (interrupted / "main").read_bytes()
    slot = int.from_bytes(later[4112:4120], "little")
    record = 4096 * (2 + slot)
    torn = bytearray(earlier) + later[len(earlier):]
    torn[record:record + 72] = later[record:record + 72]
    (interrupted / "main").write_bytes(torn)
    with probe(driver, interrupted / "main", "ro", env) as verify:
        assert verify().split()[6] == "515"
    for mixed in (False, True):
        directory = root / ("audit-index-" + str(mixed))
        run("integrity-fixture", directory, "mixed" if mixed else None)
        file = directory / "main"
        with probe(driver, file, "rw", env) as verify:
            result = verify().split()
            assert result[-2:] == ["1", str(int(not mixed))], result
            original = file.read_bytes()
            assert original.startswith(b"ZYXng004")
            def word(data, at):
                return int.from_bytes(data[at:at + 8], "little")
            pos = 112
            for _ in range(2):
                pos += 8 + word(original, pos)
            for _ in range(word(original, 96)):
                pos += 16 + word(original, pos + 8)
            # Validly resealed rows must still describe existing endpoints.
            # Indexed nodes without an integer key yield partial coverage.
            for offset, message in ((pos + 16, "dangling"), (-48, "mismatch")):
                corrupt = bytearray(original)
                corrupt[offset] ^= 64
                corrupt[-32:] = hashlib.sha256(corrupt[:-32]).digest()
                file.write_bytes(corrupt)
                diagnostic = verify()
                assert diagnostic.startswith("invalid ") and message in diagnostic, diagnostic
                file.write_bytes(original)
                assert verify().split() == result
        with probe(driver, file, "rw", env) as verify:
            assert verify("pin-stage-commit") == "pinned"
    print("Independent integrity: retained handles, selected root, segment, WAL and anchor, sealed copy, side effects passed")
