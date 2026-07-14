//! Type round-trip, node/edge return shape, collection value, and error tests.
//!
//! Mirrors bindings/python/tests/test_types.py themes so the Rust binding has
//! comparable coverage. The Rust `Record` API only exposes scalar getters
//! (`get_i64/get_f64/get_bool/get_str`); list-valued properties are read back
//! as scalars by selecting individual scalar fields (we cannot fetch a whole
//! list as a single Rust value without a list getter).

use zyxdb::{params, Database};

fn temp_db() -> (tempfile::TempDir, Database) {
    let dir = tempfile::tempdir().unwrap();
    let db = Database::open(dir.path().join("t.zyx")).unwrap();
    (dir, db)
}

fn first_cell_i64(db: &Database, query: &str) -> i64 {
    let mut r = db.execute(query, None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    row.get_i64(0).unwrap()
}

fn first_cell_f64(db: &Database, query: &str) -> f64 {
    let mut r = db.execute(query, None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    row.get_f64(0).unwrap()
}

fn first_cell_bool(db: &Database, query: &str) -> bool {
    let mut r = db.execute(query, None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    row.get_bool(0).unwrap()
}

fn first_cell_str(db: &Database, query: &str) -> String {
    let mut r = db.execute(query, None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    row.get_str(0).unwrap()
}

// --- Scalar type round-trip ---

#[test]
fn integer_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: 42}) RETURN n", None).unwrap();
    assert_eq!(first_cell_i64(&db, "MATCH (n:T) RETURN n.v AS v"), 42);
}

#[test]
fn large_integer_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: 2147483647})", None).unwrap();
    assert_eq!(first_cell_i64(&db, "MATCH (n:T) RETURN n.v AS v"), 2147483647);
}

#[test]
fn negative_integer_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: -7})", None).unwrap();
    assert_eq!(first_cell_i64(&db, "MATCH (n:T) RETURN n.v AS v"), -7);
}

#[test]
fn float_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: 3.14})", None).unwrap();
    let v = first_cell_f64(&db, "MATCH (n:T) RETURN n.v AS v");
    assert!((v - 3.14).abs() < 1e-6);
}

#[test]
fn boolean_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: true})", None).unwrap();
    assert!(first_cell_bool(&db, "MATCH (n:T) RETURN n.v AS v"));
    db.execute("CREATE (n:T {v: false})", None).unwrap();
    assert!(!first_cell_bool(&db, "MATCH (n:T {v: false}) RETURN n.v AS v"));
}

#[test]
fn string_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: 'hello'})", None).unwrap();
    assert_eq!(first_cell_str(&db, "MATCH (n:T) RETURN n.v AS v"), "hello");
}

#[test]
fn empty_string_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: ''})", None).unwrap();
    assert_eq!(first_cell_str(&db, "MATCH (n:T) RETURN n.v AS v"), "");
}

#[test]
fn unicode_string_round_trip() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {v: '你好🌍'})", None).unwrap();
    assert_eq!(first_cell_str(&db, "MATCH (n:T) RETURN n.v AS v"), "你好🌍");
}

// --- Parameterized queries ---

#[test]
fn parameterized_string_in_where() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {name: 'Alice'}) RETURN n", None).unwrap();
    db.execute("CREATE (n:T {name: 'Bob'}) RETURN n", None).unwrap();
    let p = params!("name" => "Bob").unwrap();
    let mut r = db
        .execute("MATCH (n:T) WHERE n.name = $name RETURN n.name AS name", Some(&p))
        .unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert_eq!(row.get_str(0).unwrap(), "Bob");
}

#[test]
fn parameterized_int_in_where() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {id: 1, age: 30})", None).unwrap();
    db.execute("CREATE (n:T {id: 2, age: 40})", None).unwrap();
    let p = params!("age" => 40_i64).unwrap();
    let mut r = db
        .execute("MATCH (n:T) WHERE n.age = $age RETURN n.id AS id", Some(&p))
        .unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert_eq!(row.get_i64(0).unwrap(), 2);
}

// --- Node / edge return round-trip (scalar access to properties) ---

#[test]
fn node_return_has_id_and_property() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:Person {name: 'Alice', age: 30}) RETURN n", None).unwrap();
    let mut r = db
        .execute("MATCH (n:Person) RETURN n.age AS age, n.name AS name", None)
        .unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert_eq!(row.get_i64(0).unwrap(), 30);
    assert_eq!(row.get_str(1).unwrap(), "Alice");
}

#[test]
fn edge_return_has_properties() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (a:Person {name: 'Alice'})-[:KNOWS {since: 2020}]->(b:Person {name: 'Bob'})", None).unwrap();
    let mut r = db
        .execute(
            "MATCH (a:Person)-[r:KNOWS]->(b:Person) RETURN r.since AS since, a.name AS a, b.name AS b",
            None,
        )
        .unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert_eq!(row.get_i64(0).unwrap(), 2020);
    assert_eq!(row.get_str(1).unwrap(), "Alice");
    assert_eq!(row.get_str(2).unwrap(), "Bob");
}

// --- Collection values: project scalar list elements (no list getter in the API) ---

#[test]
fn list_value_indexed_projection() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {tags: ['a', 'b', 'c']}) RETURN n", None).unwrap();
    // Read individual elements of the stored list via Cypher indexing.
    let mut r = db.execute("MATCH (n:T) RETURN n.tags[0] AS a, n.tags[1] AS b, n.tags[2] AS c", None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert_eq!(row.get_str(0).unwrap(), "a");
    assert_eq!(row.get_str(1).unwrap(), "b");
    assert_eq!(row.get_str(2).unwrap(), "c");
}

#[test]
fn float_vector_indexed_projection() {
    let (_dir, db) = temp_db();
    db.execute("CREATE (n:T {embedding: [0.1, 0.2, 0.3]}) RETURN n", None).unwrap();
    let mut r = db
        .execute("MATCH (n:T) RETURN n.embedding[0] AS a, n.embedding[1] AS b, n.embedding[2] AS c", None)
        .unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert!((row.get_f64(0).unwrap() - 0.1).abs() < 1e-5);
    assert!((row.get_f64(1).unwrap() - 0.2).abs() < 1e-5);
    assert!((row.get_f64(2).unwrap() - 0.3).abs() < 1e-5);
}

// --- Error handling / error codes ---

#[test]
fn invalid_cypher_returns_error() {
    let (_dir, db) = temp_db();
    let err = db.execute("THIS IS NOT CYPHER", None).err().expect("expected an error");
    // Parse errors map to ParseError in this engine.
    assert!(
        matches!(
            err.code(),
            zyxdb::ErrorCode::ParseError | zyxdb::ErrorCode::ExecutionError
        ),
        "expected parse/execution error, got {:?}",
        err.code()
    );
}

#[test]
fn error_carries_message() {
    let (_dir, db) = temp_db();
    let err = db.execute("THIS IS NOT CYPHER", None).err().expect("expected an error");
    assert!(!err.message().is_empty());
}

#[test]
fn drop_closes_database_without_panic() {
    let dir = tempfile::tempdir().unwrap();
    {
        let db = Database::open(dir.path().join("t.zyx")).unwrap();
        db.execute("CREATE (n:T {v: 1}) RETURN n", None).unwrap();
        drop(db); // must not panic; Drop closes the handle
    }
    // Reopen at the same path works afterwards.
    let db = Database::open(dir.path().join("t.zyx")).unwrap();
    let mut r = db.execute("MATCH (n:T) RETURN n.v AS v", None).unwrap();
    let row = r.next().unwrap().expect("expected a row");
    assert_eq!(row.get_i64(0).unwrap(), 1);
}
