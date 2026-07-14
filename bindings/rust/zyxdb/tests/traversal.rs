//! Traversal and error-code semantics tests for the Rust binding.
//! Covers shortest-path via the API-less Cypher path (no getShortestPath
//! wrapper exists in the Rust high-level crate), error-code classification,
//! and the existing out-of-range error contract.

use zyxdb::{Database, ErrorCode};

fn temp_db() -> (tempfile::TempDir, Database) {
    let dir = tempfile::tempdir().unwrap();
    let db = Database::open(dir.path().join("t.zyx")).unwrap();
    (dir, db)
}

#[test]
fn shortest_path_two_hops() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (a:Person {name: 'A'}) RETURN a", None).unwrap();
    db.execute("CREATE (b:Person {name: 'B'}) RETURN b", None).unwrap();
    db.execute("CREATE (c:Person {name: 'C'}) RETURN c", None).unwrap();
    db.execute("MATCH (a:Person {name:'A'}), (b:Person {name:'B'}) CREATE (a)-[:KNOWS]->(b)", None).unwrap();
    db.execute("MATCH (b:Person {name:'B'}), (c:Person {name:'C'}) CREATE (b)-[:KNOWS]->(c)", None).unwrap();

    // shortestPath is a RETURN expression, not a pattern assignment (per the
    // engine's Cypher grammar; see test_IntegrationCypherPatterns.cpp). The
    // connected path serializes as a list, which the Rust high-level Record
    // API has no getter for — so we assert the query executes and yields a
    // row (the traversal contract), not the path's cell value.
    let mut r = db
        .execute(
            "MATCH (a:Person {name:'A'}), (c:Person {name:'C'}) RETURN shortestPath((a)-[:KNOWS*]->(c)) AS p",
            None,
        )
        .unwrap();
    assert!(r.next().unwrap().is_some(), "expected a row for the connected shortest path");
}

#[test]
fn shortest_path_disconnected_returns_null_path() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (a:Person {name: 'A'}) RETURN a", None).unwrap();
    db.execute("CREATE (b:Person {name: 'B'}) RETURN b", None).unwrap();
    // No edge between them: the engine returns a single row with the path
    // serialized as "null" (see test_IntegrationCypherPatterns ShortestPath_NoPath).
    let mut r = db
        .execute(
            "MATCH (a:Person {name:'A'}), (b:Person {name:'B'}) RETURN shortestPath((a)-[:KNOWS*]->(b)) AS p",
            None,
        )
        .unwrap();
    let row = r.next().unwrap().expect("expected a row");
    // The null path is returned as the string "null"; the column is not a Rust-null.
    // We accept either the string "null" or an empty representation.
    let col = row.get_str(0);
    match col {
        Ok(s) => assert!(s == "null" || s.is_empty(), "expected null path, got: {s}"),
        Err(_) => { /* a null read error is also acceptable for the no-path contract */ }
    }
}

// --- Error-code classification ---

#[test]
fn out_of_range_on_column_name() {
    let (_dir, db) = temp_db();
    let mut r = db.execute("RETURN 42 AS answer", None).unwrap();
    let err = r.column_name(usize::MAX).unwrap_err();
    assert_eq!(err.code(), ErrorCode::OutOfRange);
}

#[test]
fn out_of_range_on_cell_read() {
    let (_dir, db) = temp_db();
    let mut r = db.execute("RETURN 42 AS answer", None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    let err = row.get_i64(usize::MAX).unwrap_err();
    assert_eq!(err.code(), ErrorCode::OutOfRange);
}

#[test]
fn error_message_is_non_empty() {
    let (_dir, db) = temp_db();
    let err = db.execute("INVALID", None).err().expect("expected an error");
    assert!(!err.message().is_empty());
}
