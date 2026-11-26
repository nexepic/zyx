"""Exercise one transaction across multiple ordinary ZYX WAL I/O windows."""
import hashlib
import shutil
import signal
import subprocess

def exercise(root, new, driver, env):
    killed = -signal.SIGKILL

    def run(op, path, rows=90, fault=None, expected=0):
        args = [driver, op, str(path), str(rows)]
        if fault:
            args.append(fault)
        p = subprocess.run(args, env=env, text=True, capture_output=True, timeout=120)
        assert p.returncode == expected, (args, expected, p.returncode, p.stdout, p.stderr)
        return p

    seed = new("stream-seed")
    run("stream-seed", seed)
    def clone(name, source=seed):
        return shutil.copytree(source.parent, root / ("stream-" + name)) / "main"
    run("stream-duplicate", clone("unique"))
    live = clone("pin")
    run("stream-life", live)
    run("stream-read", live)
    run("stream-read", seed, 0)

    # A binary node marker and an independently typed binary relationship
    # marker have distinct byte patterns. Read them without any large rows.
    binary = clone("bytes")
    run("stream-read", binary, 0)
    run("stream-export", binary)
    run("stream-read", binary.with_suffix(".view"), 0)

    def budget_write(cap, prefix):
        path = clone("budget")
        attempt = subprocess.run([driver, prefix + "write", path, str(cap)],
                                 env=env, text=True, capture_output=True, timeout=30)
        if attempt.returncode:
            assert "budget" in attempt.stderr, attempt.stderr
            assert path.read_bytes() == seed.read_bytes()
        else:
            run(prefix + "read", path, cap)
        shutil.rmtree(path.parent)
        return attempt.returncode == 0

    for prefix, lo, hi in (("stream-budget-", 1 << 20, 4 << 20),
                           ("stream-budget-small-", 512, 16384)):
        assert not budget_write(lo, prefix) and budget_write(hi, prefix)
        while hi - lo > 1:
            mid = (hi + lo) // 2
            if budget_write(mid, prefix):
                hi = mid
            else:
                lo = mid
        print(prefix, "same-budget ACK and independent reopen:", hi, "rejected:", lo)

    for mode, tag in (("stream-write", "w"), ("stream-append", "a")):
        good = clone(tag + "-ACK")
        run(mode, good, expected=killed)
        log = good.with_suffix(".wal").read_bytes()
        offset = log.rfind(b"ZYXtx002")
        # In a new generation the fixed 72-byte header shares the first write.
        start = 0 if tag == "w" else offset
        assert offset >= 72 and len(log) - start > 2 * 1024 * 1024
        windows = (len(log) - start + 32767) // 32768
        for ordinal in (1, windows // 2, windows):
            path = clone(tag + "-window-" + str(ordinal))
            run(mode, path, fault=f"{tag}.window={ordinal}", expected=2)
            run("stream-read", path, 0)
        for event, code, count in [(tag + ".sync=eio", 2, 0), ("m.sync=eio", 2, 0),
                                   ("m.rename=kill", killed, 0), ("m.renamed=kill", killed, 90)]:
            path = clone(tag + "-" + event)
            run(mode, path, fault=event, expected=code)
            run("stream-read", path, count)
        for kind in ("truncated", "late-invalid", "duplicate"):
            path = clone(tag + "-" + kind, good)
            wal = path.with_suffix(".wal")
            data = bytearray(log)
            receipt = bytearray(path.read_bytes())
            if kind == "truncated":
                del data[-1]
            else:
                if kind == "late-invalid":
                    assert data[-1] == 0
                    data[-1] = 1  # Invalid padding after processing the rows.
                else:
                    # Rewrite the last unique key to the first key, retaining
                    # canonical integer encoding and a matching receipt digest.
                    key = b'i' + (14).to_bytes(8,'little') + bytes.fromhex("03 00 08 00 00 00")
                    old = key + (89).to_bytes(8,'little')
                    assert data.count(old) == 1
                    data = data.replace(old, key + bytes(8))
                assert receipt[48:80] == hashlib.sha256(log).digest()
                receipt[48:80] = hashlib.sha256(data).digest()
                receipt[80:] = hashlib.sha256(receipt[:80]).digest()
                path.write_bytes(receipt)
            wal.write_bytes(data)
            failure = run("stream-read", path, expected=2)
            if kind == "duplicate":
                assert "duplicate key" in failure.stderr, failure.stderr
            assert path.read_bytes() == receipt and wal.read_bytes() == data
        if tag == "w":
            export_source = clone("export", good)
            run("stream-export", export_source)
            run("stream-read", export_source.with_suffix(".view"))
        run("stream-read", good)
        print(tag, "fault windows", (1, windows // 2, windows), "and transaction validation passed")

    for mode in ("stream-write", "stream-roll"):
        large = clone(mode + "-720")
        result = run(mode, large, 720, expected=killed)
        assert int(result.stdout.split("wal_bytes ")[1].split()[0]) > 16 * 1024 * 1024
        run("stream-read", large, 720)
    print("90 and 720 x 24 KiB: uncheckpointed replay, automatic rotation, old read and full value verification passed")
