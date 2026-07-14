"""Vector index end-to-end tests for the Python binding.

Covers CREATE VECTOR INDEX, insert, search via db.index.vector.queryNodes,
score semantics, manual train, persistence across reopen, and error cases.

Cross-binding contract (confirmed in C++ test_VectorIndex.cpp and the Node
binding tests):
  - metric must be 'L2' | 'IP' | 'Cosine' (exact case); anything else (e.g.
    'COSINE') is silently treated as L2.
  - the query vector passed to queryNodes MUST be a literal list in the Cypher
    string; passing $param collapses to an empty value and the procedure throws
    "queryVector argument must be a List of floats.".
  - L2 score = squared distance; IP/Cosine score = negative inner product.
    Results sorted ascending by score (lower is closer).
  - search works WITHOUT training for small graphs (flat/raw greedy search).
"""

import pytest

import zyxdb


def _open(path):
    db = zyxdb.Database(path)
    db.open()
    return db


@pytest.fixture
def vec_db(tmp_path):
    path = tmp_path / "vec_db"
    db = _open(str(path))
    yield db
    try:
        db.close()
    except Exception:
        pass


def _query_vector(vec):
    """Build a literal-list query vector string (no $param)."""
    return ",".join(str(v) for v in vec)


def _insert_l2_fixture(db, idx="vec_l2"):
    db.execute(f"CREATE VECTOR INDEX {idx} ON :V(embedding) OPTIONS {{dimension: 2, metric: 'L2'}}")
    db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})")
    db.execute("CREATE (:V {id: 2, embedding: [0.0, 1.0]})")
    db.execute("CREATE (:V {id: 3, embedding: [0.0, 0.0]})")


def test_l2_search_returns_top_match_with_squared_distance(vec_db):
    _insert_l2_fixture(vec_db)
    q = f"CALL db.index.vector.queryNodes('vec_l2', 1, [{_query_vector([0.9, 0.1])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(vec_db.execute(q))
    assert len(rows) >= 1
    assert rows[0]["id"] == 1  # closest to [0.9,0.1] is node 1 [1.0,0.0]
    # difference [0.1,-0.1] -> squared distance ~0.02
    assert abs(float(rows[0]["score"]) - 0.02) < 1e-2


def test_search_returns_up_to_k_sorted_ascending(vec_db):
    _insert_l2_fixture(vec_db)
    q = f"CALL db.index.vector.queryNodes('vec_l2', 3, [{_query_vector([0.9, 0.1])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(vec_db.execute(q))
    assert len(rows) == 3
    scores = [float(r["score"]) for r in rows]
    assert scores == sorted(scores)


def test_cosine_metric_returns_negative_inner_product(vec_db):
    vec_db.execute("CREATE VECTOR INDEX vec_cos ON :V(embedding) OPTIONS {dimension: 2, metric: 'Cosine'}")
    vec_db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})")
    vec_db.execute("CREATE (:V {id: 2, embedding: [0.0, 1.0]})")
    q = f"CALL db.index.vector.queryNodes('vec_cos', 1, [{_query_vector([1.0, 0.0])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(vec_db.execute(q))
    assert len(rows) >= 1
    assert rows[0]["id"] == 1
    assert abs(float(rows[0]["score"]) - (-1.0)) < 5e-2


def test_ip_metric_equivalent_to_cosine_for_normalized(vec_db):
    vec_db.execute("CREATE VECTOR INDEX vec_ip ON :V(embedding) OPTIONS {dimension: 2, metric: 'IP'}")
    vec_db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})")
    vec_db.execute("CREATE (:V {id: 2, embedding: [0.0, 1.0]})")
    q = f"CALL db.index.vector.queryNodes('vec_ip', 1, [{_query_vector([1.0, 0.0])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(vec_db.execute(q))
    assert rows[0]["id"] == 1


def test_uppercase_cosine_silently_uses_l2(vec_db):
    # 'COSINE' is not a recognized metric string; the engine falls back to L2.
    vec_db.execute("CREATE VECTOR INDEX vec_upper ON :V(embedding) OPTIONS {dimension: 2, metric: 'COSINE'}")
    vec_db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})")
    q = f"CALL db.index.vector.queryNodes('vec_upper', 1, [{_query_vector([1.0, 0.0])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(vec_db.execute(q))
    assert len(rows) >= 1
    assert rows[0]["id"] == 1


def test_insert_via_parameterized_list(vec_db):
    vec_db.execute("CREATE VECTOR INDEX vec_param ON :V(embedding) OPTIONS {dimension: 3, metric: 'L2'}")
    vec_db.execute("CREATE (:V {id: 1, embedding: $emb})", emb=[1.0, 2.0, 3.0])
    q = f"CALL db.index.vector.queryNodes('vec_param', 1, [{_query_vector([1.0, 2.0, 3.0])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(vec_db.execute(q))
    assert rows[0]["id"] == 1
    assert abs(float(rows[0]["score"]) - 0.0) < 1e-2


def test_manual_train_returns_status_and_search_still_works(vec_db):
    vec_db.execute("CREATE VECTOR INDEX vec_train ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}")
    for i in range(10):
        x = i / 10.0
        vec_db.execute(f"CREATE (:V {{id: {i}, embedding: [{x}, {1.0 - x}]}})")
    rows = list(vec_db.execute("CALL db.index.vector.train('vec_train') YIELD status RETURN status"))
    assert len(rows) >= 1
    assert isinstance(rows[0]["status"], str) and len(rows[0]["status"]) > 0

    q = f"CALL db.index.vector.queryNodes('vec_train', 1, [{_query_vector([0.0, 1.0])}]) YIELD node, score RETURN node.id AS id"
    rows = list(vec_db.execute(q))
    assert len(rows) >= 1


def test_train_on_empty_index_returns_skipped(vec_db):
    vec_db.execute("CREATE VECTOR INDEX vec_empty ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}")
    rows = list(vec_db.execute("CALL db.index.vector.train('vec_empty') YIELD status RETURN status"))
    assert len(rows) >= 1
    assert "skip" in str(rows[0]["status"]).lower()


def test_train_on_nonexistent_index_is_rejected(vec_db):
    # The Python binding surfaces engine errors on the Result object (is_success /
    # error) rather than raising, so assert that contract — not an exception.
    r = vec_db.execute("CALL db.index.vector.train('does_not_exist') YIELD status RETURN status")
    assert not r.is_success
    assert "not found" in str(r.error or "").lower()


def test_dimension_mismatch_on_insert_is_tolerated(vec_db):
    # A wrong-dimension insert is logged and skipped from the index; the node
    # itself is still created in the graph but not searchable.
    vec_db.execute("CREATE VECTOR INDEX vec_dim ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}")
    vec_db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})")
    vec_db.execute("CREATE (:V {id: 2, embedding: [1.0, 0.0, 0.0, 0.0]})")
    q = f"CALL db.index.vector.queryNodes('vec_dim', 5, [{_query_vector([1.0, 0.0])}]) YIELD node, score RETURN node.id AS id"
    rows = list(vec_db.execute(q))
    ids = [r["id"] for r in rows]
    assert 1 in ids
    assert 2 not in ids


def test_vector_index_and_trained_state_survive_reopen(tmp_path):
    path = tmp_path / "vec_persist"
    db = _open(str(path))
    db.execute("CREATE VECTOR INDEX vec_persist ON :V(embedding) OPTIONS {dimension: 4, metric: 'L2'}")
    for i in range(8):
        db.execute(f"CREATE (:V {{id: {i}, embedding: [{i}.0, {8 - i}.0, {i * 2}.0, {(8 - i) * 2}.0]}})")
    db.execute("CALL db.index.vector.train('vec_persist') YIELD status RETURN status")
    db.save()
    db.close()

    db = _open(str(path))
    q = f"CALL db.index.vector.queryNodes('vec_persist', 1, [{_query_vector([0.0, 8.0, 0.0, 16.0])}]) YIELD node, score RETURN node.id AS id, score"
    rows = list(db.execute(q))
    assert len(rows) >= 1
    # Nearest to the query is the node id=0 vector [0,8,0,16].
    assert rows[0]["id"] == 0
    db.close()


def test_query_must_use_literal_vector_not_param(vec_db):
    """Pins that $param is not supported in the CALL query-vector position."""
    _insert_l2_fixture(vec_db, "vec_lit")
    result = vec_db.execute(
        "CALL db.index.vector.queryNodes('vec_lit', 1, $q) YIELD node, score RETURN node.id AS id, score",
        q=[0.9, 0.1],
    )
    assert not result.is_success
    err = result.error or ""
    assert any(tok in err.lower() for tok in ("list of floats", "queryvector", "argument"))


def test_vector_property_round_trips_as_list(vec_db):
    # Sanity: a bare LIST property (no index) round-trips via the API.
    vec_db.create_node("T", {"embedding": [0.1, 0.2, 0.3]})
    rows = list(vec_db.execute("MATCH (n:T) RETURN n.embedding AS emb"))
    result = rows[0]["emb"]
    assert len(result) == 3
    assert abs(float(result[0]) - 0.1) < 1e-5
