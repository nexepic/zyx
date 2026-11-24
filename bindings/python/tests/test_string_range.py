"""Unrepresentable string index keys remain visible to ordered filters."""
def test_range_includes_property_too_large_for_an_index_key(db):
    for value in ("a", "m" * 5000, "z"):
        assert db.execute("CREATE (:Sample {v:$v})", v=value).is_success
    assert db.execute("CREATE INDEX sample_value FOR (n:Sample) ON (n.v)").is_success
    query = "MATCH (n:Sample) WHERE n.v >= {} AND n.v < {} RETURN count(*) AS n"
    for lower, upper, params in [("'a'", "'z'", {}), ("$lo", "'z'", {"lo": "a"}),
                                 ("'a'", "$hi", {"hi": "z"}), ("$lo", "$hi", {"lo": "a", "hi": "z"})]:
        result = db.execute(query.format(lower, upper), **params)
        assert result.is_success, result.error
        assert [row["n"] for row in result] == [2]


def test_open_numeric_lower_bound_can_select_long_strings(db):
    for key, value in enumerate([1, "a", "m" * 5000, "z"]):
        assert db.execute("CREATE (:Sample {k:$k, v:$v})", k=key, v=value).is_success
    assert db.execute("CREATE INDEX sample_value FOR (n:Sample) ON (n.v)").is_success
    for condition, params, expected in [("n.v >= 0", {}, [0, 1, 2, 3]),
                                        ("n.v >= 0 AND n.v < $hi", {"hi": "z"}, [0, 1, 2])]:
        result = db.execute("MATCH (n:Sample) WHERE " + condition + " RETURN n.k AS k ORDER BY k", **params)
        assert result.is_success, result.error
        assert [row["k"] for row in result] == expected
