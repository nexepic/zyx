"""Recovery picks a confirmation before authenticating the named root."""


def exercise(root, new, run):
    main = new("embedded-history")
    assert run("embedded-batch", main, "512") == "512"
    assert run("embedded-batch", main, "1") == "513"
    earlier = main.read_bytes()
    assert run("embedded-batch", main, "1") == "514"
    latest = main.read_bytes()
    assert latest.startswith(b"ZYXrt002") and len(latest) > len(earlier)
    assert list(main.parent.iterdir()) == [main]

    def cold(name, contents, expected):
        target = root / name
        target.mkdir()
        path = target / "main"
        path.write_bytes(contents)
        if expected is None:
            run("count", path, code=2)
            assert path.read_bytes() == contents
        else:
            assert run("count", path) == str(expected)

    # On acknowledgement, two separately fenced receipts confirm the same
    # root. One valid but stale receipt must not rewind that acknowledgement.
    for pos in (4096, 16384):
        copy = bytearray(latest)
        copy[pos:pos + 88] = earlier[pos:pos + 88]
        cold("stale-receipt-" + str(pos), copy, 514)

    slot = int.from_bytes(latest[4112:4120], "little")
    record = 4096 * (2 + slot)
    copy = bytearray(latest)
    copy[record + 40] ^= 1
    cold("corrupt-confirmed-root", copy, None)
    copy = bytearray(latest)
    copy[-1] ^= 1
    cold("corrupt-confirmed-data", copy, None)

    # Incomplete writes to the inactive slot cannot change what either
    # previous confirmation names. The appended file tail carries no vote.
    copy = bytearray(earlier) + latest[len(earlier):]
    copy[record:record + 72] = latest[record:record + 72]
    cold("torn-unconfirmed-root", copy, 513)

    # Bind the index, data and base UUID by checking the selected root
    # against the base header, including same-size, same-revision databases.
    foreign = new("foreign-history")
    for size in ("512", "1", "1"):
        run("embedded-batch", foreign, size)
    other = foreign.read_bytes()
    copy = bytearray(latest)
    for pos, length in ((4096, 88), (16384, 88), (record, 144)):
        copy[pos:pos + length] = other[pos:pos + length]
    cold("foreign-confirmed-root", copy, None)

    copy = bytearray(latest)
    for pos in (4096, 16384):
        copy[pos:pos + 88] = earlier[pos:pos + 88]
    cold("both-confirmations-stale", copy, None)

    # A separately confirmed ordinary WAL remains authoritative until its
    # anchor is promoted. Each process reopens through the official name.
    import shutil
    import signal
    prior = new("seal-source")
    run("embedded-batch", prior, "1000")
    run("seal-prefix", prior)
    for event, code in (
        ("m.dirsynced=kill", -signal.SIGKILL), ("s.data=partial-eio", 2),
        ("s.root=partial-eio", 2), ("s.first=partial-eio", 2),
        ("s.second=partial-eio", 2), ("s.retire=partial-eio", 2),
        ("s.sync=eio", 2), ("s.synced=kill", -signal.SIGKILL),
        ("s.rename=eio", 2), ("s.renamed=kill", -signal.SIGKILL),
        ("s.dirsync=eio", 2), ("w.unlink=eio", 2),
        ("w.unlinked=kill", -signal.SIGKILL),
    ):
        clone = root / ("seal-" + event)
        shutil.copytree(prior.parent, clone)
        run("seal-batch", clone / "main", event, code)
        assert run("count", clone / "main") == "1001"
        run("seal-batch", clone / "main")
        assert run("count", clone / "main") == "1002"
    run("seal-batch", prior)
    assert list(prior.parent.iterdir()) == [prior]
    cold("seal-complete-copy", prior.read_bytes(), 1002)

    # A transition receipt still requires both the exact WAL and the old
    # immutable row extent. Intact new control bytes cannot substitute.
    pending = new("seal-pending")
    run("embedded-batch", pending, "1000")
    run("seal-prefix", pending)
    run("seal-batch", pending, "s.synced=kill", -signal.SIGKILL)
    assert pending.stat().st_size == 256
    for name in ("missing-wal", "truncated-wal", "damaged-header", "damaged-row", "foreign-anchor"):
        folder = root / ("seal-invalid-" + name)
        shutil.copytree(pending.parent, folder)
        path = folder / "main"
        wal, anchor = path.with_suffix(".wal"), path.with_suffix(".base")
        if name == "missing-wal":
            wal.unlink()
        elif name == "truncated-wal":
            wal.write_bytes(wal.read_bytes()[:-1])
        elif name == "foreign-anchor":
            anchor.write_bytes(prior.read_bytes())
        else:
            target, offset = (path, 96) if name == "damaged-header" else (anchor, 20480 + 256)
            wire = bytearray(target.read_bytes())
            wire[offset] ^= 1
            target.write_bytes(wire)
        before = path.read_bytes()
        run("count", path, code=2)
        assert path.read_bytes() == before

    # Readable prior data need not be writable, and promotion must not
    # reintroduce a prior inode's broader access at the official path.
    for permissions in (0o400, 0o644):
        restricted = new("seal-access-" + str(permissions))
        run("embedded-batch", restricted, "1000")
        run("seal-prefix", restricted)
        restricted.with_suffix(".base").chmod(permissions)
        run("seal-batch", restricted)
        assert restricted.stat().st_mode & 0o777 == 0o600
        assert run("count", restricted) == "1002"
