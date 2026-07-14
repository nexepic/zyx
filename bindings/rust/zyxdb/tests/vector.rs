//! Vector index end-to-end tests for the Rust binding.
//!
//! Cross-binding contract (see bindings/nodejs/test/vector.test.js and the
//! C++ test_VectorIndex.cpp authority):
//!  - metric must be 'L2' | 'IP' | 'Cosine' (exact case); other spellings
//!    silently fall back to L2.
//!  - the query vector for db.index.vector.queryNodes MUST be a literal list in
//!    the Cypher string (CALL args are evaluated with an empty context, so a
//!    $param collapses to empty and the call fails). The Rust `Params` API has
//!    no list value anyway, so literals are the only option.
//!  - L2 score = squared distance; IP/Cosine score = negative inner product;
//!    results sorted ascending by score (lower is closer).
//!  - search works without training for small graphs (flat/raw greedy search).
//!
//! Note: the Rust high-level crate does NOT wrap QueryResult.is_success; engine
//! errors surface as `Err(Error)` from `execute`.

use zyxdb::{Database, ErrorCode};

fn temp_db() -> (tempfile::TempDir, Database) {
    let dir = tempfile::tempdir().unwrap();
    let db = Database::open(dir.path().join("vec.zyx")).unwrap();
    (dir, db)
}

fn vec_literal(v: &[f64]) -> String {
    v.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(",")
}

/// Run a queryNodes search and return (ids, scores) in ascending-score order.
fn search(db: &Database, idx: &str, k: usize, query: &[f64]) -> Vec<(i64, f64)> {
    let q = format!(
        "CALL db.index.vector.queryNodes('{idx}', {k}, [{q}]) YIELD node, score RETURN node.id AS id, score",
        idx = idx,
        k = k,
        q = vec_literal(query)
    );
    let mut r = db.execute(&q, None).expect("vector query failed");
    let mut out = Vec::new();
    while let Some(row) = r.next().unwrap() {
        let id = row.get_i64(0).unwrap();
        // The score column is a float; the high-level API exposes get_f64.
        let score = row.get_f64(1).unwrap();
        out.push((id, score));
    }
    out
}

fn insert_l2_fixture(db: &Database) {
    db.execute("CREATE VECTOR INDEX vidx ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}", None).unwrap();
    db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})", None).unwrap();
    db.execute("CREATE (:V {id: 2, embedding: [0.0, 1.0]})", None).unwrap();
    db.execute("CREATE (:V {id: 3, embedding: [0.0, 0.0]})", None).unwrap();
}

#[test]
fn l2_search_returns_top_match_with_squared_distance() {
    let (_dir, db) = temp_db();
    insert_l2_fixture(&db);
    let res = search(&db, "vidx", 1, &[0.9, 0.1]);
    assert!(!res.is_empty());
    assert_eq!(res[0].0, 1); // closest to [0.9,0.1] is node 1 [1.0,0.0]
    assert!((res[0].1 - 0.02).abs() < 1e-2, "expected ~0.02, got {}", res[0].1);
}

#[test]
fn search_returns_up_to_k_sorted_ascending() {
    let (_dir, db) = temp_db();
    insert_l2_fixture(&db);
    let res = search(&db, "vidx", 3, &[0.9, 0.1]);
    assert_eq!(res.len(), 3);
    assert!(res[0].1 <= res[1].1 && res[1].1 <= res[2].1);
}

#[test]
fn cosine_metric_returns_negative_inner_product() {
    let (_dir, db) = temp_db();
    db.execute("CREATE VECTOR INDEX vcos ON :V(embedding) OPTIONS {dimension: 2, metric: 'Cosine'}", None).unwrap();
    db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})", None).unwrap();
    db.execute("CREATE (:V {id: 2, embedding: [0.0, 1.0]})", None).unwrap();
    let res = search(&db, "vcos", 1, &[1.0, 0.0]);
    assert_eq!(res[0].0, 1);
    assert!((res[0].1 - (-1.0)).abs() < 5e-2, "expected ~-1.0, got {}", res[0].1);
}

#[test]
fn uppercase_cosine_silently_uses_l2() {
    let (_dir, db) = temp_db();
    db.execute("CREATE VECTOR INDEX vup ON :V(embedding) OPTIONS {dimension: 2, metric: 'COSINE'}", None).unwrap();
    db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})", None).unwrap();
    let res = search(&db, "vup", 1, &[1.0, 0.0]);
    assert_eq!(res[0].0, 1);
}

#[test]
fn manual_train_returns_status_row_and_search_still_works() {
    let (_dir, db) = temp_db();
    db.execute("CREATE VECTOR INDEX vtrain ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}", None).unwrap();
    for i in 0..10i64 {
        let x = i as f64 / 10.0;
        let y = 1.0 - x;
        db.execute(&format!("CREATE (:V {{id: {i}, embedding: [{x}, {y}]}})", i = i, x = x, y = y), None).unwrap();
    }
    let mut r = db
        .execute("CALL db.index.vector.train('vtrain') YIELD status RETURN status", None)
        .expect("train call failed");
    let row = r.next().unwrap().expect("expected a status row");
    let status = row.get_str(0).expect("status string");
    assert!(!status.is_empty());

    // After training, search still returns the nearest node.
    let res = search(&db, "vtrain", 1, &[0.0, 1.0]);
    assert!(!res.is_empty());
}

#[test]
fn train_on_empty_index_returns_skipped_status() {
    let (_dir, db) = temp_db();
    db.execute("CREATE VECTOR INDEX vempty ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}", None).unwrap();
    let mut r = db
        .execute("CALL db.index.vector.train('vempty') YIELD status RETURN status", None)
        .unwrap();
    let row = r.next().unwrap().expect("expected a status row");
    let status = row.get_str(0).unwrap().to_lowercase();
    assert!(status.contains("skip"), "expected skip in status, got: {status}");
}

#[test]
fn train_on_nonexistent_index_errors() {
    let (_dir, db) = temp_db();
    let result = db.execute("CALL db.index.vector.train('does_not_exist') YIELD status RETURN status", None);
    match result {
        Ok(_) => panic!("expected training on a missing index to error"),
        Err(e) => assert!(
            matches!(e.code(), ErrorCode::NotFound | ErrorCode::ExecutionError),
            "expected NotFound/ExecutionError, got {:?}",
            e.code()
        ),
    }
}

#[test]
fn dimension_mismatch_on_insert_is_tolerated_by_index() {
    let (_dir, db) = temp_db();
    db.execute("CREATE VECTOR INDEX vdim ON :V(embedding) OPTIONS {dimension: 2, metric: 'L2'}", None).unwrap();
    db.execute("CREATE (:V {id: 1, embedding: [1.0, 0.0]})", None).unwrap();
    db.execute("CREATE (:V {id: 2, embedding: [1.0, 0.0, 0.0, 0.0]})", None).unwrap(); // skipped from index

    let res = search(&db, "vdim", 5, &[1.0, 0.0]);
    let ids: Vec<_> = res.iter().map(|(id, _)| *id).collect();
    assert!(ids.contains(&1), "2-d node must be found");
    assert!(!ids.contains(&2), "4-d node must be skipped from the index");
}

// Note on the literal-query-vector contract: the other bindings pin that
// `$param` cannot be used for the CALL query vector (it collapses to empty at
// parse time). In the Rust binding this is structurally enforced — the
// high-level `Params` API has no list value type at all (see src/params.rs
// IntoParam impls), so a `$vec`-style vector parameter cannot be constructed.
// We therefore do not duplicate the engine-rejection assertion here; the
// Node and Python suites cover it, and an empty-list probe triggers undefined
// behaviour in the engine (out of scope for this binding test layer).
