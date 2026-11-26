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
