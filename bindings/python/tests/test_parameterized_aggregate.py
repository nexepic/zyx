"""Parameterized filters preserve grouped results while using direct batches."""


def _query_rows_and_profile(db, query, **parameters):
    result = db.execute("PROFILE " + query, **parameters)
    assert result.is_success, result.error
    rows = list(result)
    profile = next(row for row in rows if row["phase"] == "logical_plan")
    output = {row["grp"]: row["n"] for row in rows if row["phase"] is None}
    return output, profile


def test_grouped_filter_rebinds_runtime_parameters(db):
    created = db.execute(
        "CREATE (:Item {grp:'a', val:1}), (:Item {grp:'a', val:2}), "
        "(:Item {grp:'a', val:3}), (:Item {grp:'b', val:4}), "
        "(:Item {grp:'b', val:4}), (:Item {grp:'b', val:2.5}), "
        "(:Item {grp:'b', val:null}), (:Item {grp:'b'})"
    )
    assert created.is_success, created.error

    output_clause = " RETURN i.grp AS grp, count(*) AS n"
    direct = "MATCH (i:Item) WHERE i.val >= $lo AND i.val <> $skip" + output_clause
    # An expression RHS keeps the comparison on the row evaluator. COALESCE
    # with NULL preserves each parameter, including an explicitly bound NULL.
    row = (
        "MATCH (i:Item) WHERE i.val >= coalesce($lo, null) "
        "AND i.val <> coalesce($skip, null)" + output_clause
    )
    for lo, skip, expected in (
        (1, 2, {"a": 2, "b": 3}),
        (3, 4, {"a": 1, "b": 1}),
        (2.5, 4.0, None),  # type-sensitive numeric comparisons: use the row oracle
        ("two", True, None),
        (None, 9, {}),
        (1, None, {}),
        (1, 2, {"a": 2, "b": 3}),
    ):
        actual, profile = _query_rows_and_profile(db, direct, lo=lo, skip=skip)
        fallback, row_profile = _query_rows_and_profile(db, row, lo=lo, skip=skip)
        assert row_profile["access_path"] == "bounded_property_row_adapter"
        assert actual == fallback
        if expected is not None:
            assert actual == expected
        assert profile["access_path"] == "direct_property_batches"
        assert profile["physical_work"]["logical_plan.direct_property.batches"] > 0
        assert profile["physical_work"].get("logical_plan.aggregate.direct_input_rows", 0) == sum(actual.values())


def test_grouped_short_text_filter_rebinds_in_direct_batches(db):
    created = db.execute(
        "CREATE (:Item {grp:'aa'}), (:Item {grp:'bb'}), (:Item {grp:'bb'}), "
        "(:Item {grp:'cc'}), (:Item {grp:null}), (:Item)"
    )
    assert created.is_success, created.error
    query = "MATCH (i:Item) WHERE i.grp = $grp RETURN i.grp AS grp, count(*) AS n"
    for grp, expected in (("aa", {"aa": 1}), ("bb", {"bb": 2}), ("", {}), (None, {}), ("cc", {"cc": 1})):
        output, profile = _query_rows_and_profile(db, query, grp=grp)
        assert output == expected
        assert profile["access_path"] == "direct_property_batches"


def test_grouped_filter_missing_parameter_keeps_row_error_timing(db):
    assert db.execute("CREATE (:Item {grp:'a', val:1})").is_success
    body = " WHERE i.val >= $bound RETURN i.grp AS grp, count(*) AS n"
    result = db.execute("MATCH (i:Item)" + body)
    assert not result.is_success
    assert "parameter" in result.error
    empty = db.execute("MATCH (i:NoSuchLabel)" + body)
    assert empty.is_success, empty.error
    assert list(empty) == []
