"""Hold the real writer at namespace transitions and compete through fresh SDK processes."""
import os
import shutil
import signal
import subprocess
import time


def stopped(process):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        pid, status = os.waitpid(process.pid, os.WUNTRACED | os.WNOHANG)
        if pid:
            assert os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGSTOP, status
            return
        time.sleep(.01)
    raise AssertionError("writer did not stop at namespace transition")


def exercise_isolation(root, new, run, recovered, driver, env):
    # A clean byte-for-byte copy deliberately shares the persisted graph ID.
    # Two official names must never share a stage even when one is mid-encode.
    for tag, op, event, crash in (
        ("stage", "locked-checkpoint", "m.sync=stop", False),
        ("published", "locked-checkpoint", "m.renamed=stop", False),
        ("cleanup", "locked-close", "", False),
        ("crash", "locked-checkpoint", "m.renamed=stop", True),
    ):
        main = new("lock-" + tag)
        other = main.parent / "second"
        shutil.copyfile(main, other)
        alias = root / ("alias-" + tag)
        alias.symlink_to(main.parent, target_is_directory=True)
        args = [driver, op, str(main)] + ([event] if event else [])
        with subprocess.Popen(args, env=env) as writer:
            try:
                stopped(writer)
                run("append", other)
                recovered(other, 1)
                for path in (main, alias / "main", main.parent / ".." / main.parent.name / "main"):
                    run("append", path, code=2, error="lock native graph main")
                run("read-only", main, code=2, error="lock native graph main")
                external = root / ("external-" + tag)
                os.link(main, external)
                try:
                    run("append", external, code=2, error="private, unaliased files")
                finally:
                    external.unlink()
                if crash:
                    writer.kill()
                else:
                    os.kill(writer.pid, signal.SIGCONT)
                assert writer.wait(timeout=25) == (-signal.SIGKILL if crash else 0)
            finally:
                if writer.poll() is None:
                    writer.kill()
        recovered(main, 1)
        run("append", main)
        recovered(main, 2)
        assert {f.name for f in main.parent.iterdir()} == {"main", "second"}
        print("Independent copy committed, same main excluded, later takeover verified:", tag)

    main = new("stale-opener")
    paused_env = {**env, "ZYX_TEST_NATIVE_IO": "f.admit=stop"}
    with subprocess.Popen([driver, "append", str(main)], env=paused_env,
                          stderr=subprocess.PIPE, text=True) as stale:
        try:
            stopped(stale)
            run("append", main)
            os.kill(stale.pid, signal.SIGCONT)
            _, error = stale.communicate(timeout=20)
            assert stale.returncode == 2 and "base authority changed" in error, error
        finally:
            if stale.poll() is None:
                stale.kill()
    recovered(main, 1)
    print("Opener paused before flock rejected after another process replaced the main")

    # There is no lock sidecar. A leftover similarly named file, its inode
    # replacement, and its removal must not affect the genuine kernel lock.
    main = new("residual-lock-name")
    extra = main.with_suffix(".lock")
    extra.touch()
    with subprocess.Popen([driver, "hold", str(main)], env=env, stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, text=True) as holder:
        assert holder.stdout.readline().strip() == "locked"
        replacement = main.with_suffix(".replacement")
        replacement.touch()
        os.replace(replacement, extra)
        run("append", main, code=2, error="lock native graph main")
        extra.unlink()
        run("append", main, code=2, error="lock native graph main")
        holder.communicate("\n", timeout=10)
    run("append", main)
    recovered(main, 1)
    print("Unrelated .lock replacement/removal did not admit a second writer")

    # A previous writer could die leaving its ID-only stage behind. Current
    # stage names use a disjoint namespace, even for identical graph copies.
    main = new("legacy-stage")
    abandoned = main.parent / (".zyx-native-" + main.read_bytes()[8:24].hex())
    abandoned.write_bytes(b"partial checkpoint")
    run("read-only", main)
    assert abandoned.exists()
    recovered(main, 0)
    assert list(main.parent.iterdir()) == [main]
    print("Authenticated writable recovery removed the abandoned legacy stage")

    main = new("failed-fence")
    with subprocess.Popen([driver, "hold-failed-fence", str(main)], env=env,
                          stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True) as writer:
        try:
            assert writer.stdout.readline().strip() == "failed fence"
            run("append", main, code=2, error="lock native graph main")
        finally:
            writer.kill()
            writer.wait(timeout=10)
    recovered(main, 1)
    print("Failed post-rename directory fence retained exclusion until process death")
