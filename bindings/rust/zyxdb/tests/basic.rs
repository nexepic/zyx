use zyxdb::{params, Database, Params};

#[test]
fn executes_scalar_query() -> zyxdb::Result<()> {
    let tempdir = tempfile::tempdir().unwrap();
    let db_path = tempdir.path().join("test.zyx");
    let db = Database::open(&db_path)?;

    let mut result = db.execute(
        "RETURN 42 AS answer, 3.5 AS ratio, true AS ok, 'zyx' AS name",
        None,
    )?;
    assert_eq!(result.column_count(), 4);
    assert_eq!(result.column_name(0)?, "answer");

    let row = result.next()?.expect("expected one row");
    assert_eq!(row.get_i64(0)?, 42);
    assert_eq!(row.get_f64(1)?, 3.5);
    assert!(row.get_bool(2)?);
    assert_eq!(row.get_str(3)?, "zyx");
    drop(row);
    assert!(result.next()?.is_none());

    Ok(())
}

#[test]
fn executes_query_with_params() -> zyxdb::Result<()> {
    let tempdir = tempfile::tempdir().unwrap();
    let db_path = tempdir.path().join("test.zyx");
    let db = Database::open(&db_path)?;
    let params = params!(
        "answer" => 42_i64,
        "ratio" => 2.5_f64,
        "ok" => true,
        "name" => "neo",
    );

    let mut result = db.execute(
        "RETURN $answer AS answer, $ratio AS ratio, $ok AS ok, $name AS name",
        Some(&params),
    )?;

    let row = result.next()?.expect("expected one row");
    assert_eq!(row.get_i64(0)?, 42);
    assert_eq!(row.get_f64(1)?, 2.5);
    assert!(row.get_bool(2)?);
    assert_eq!(row.get_str(3)?, "neo");

    Ok(())
}

#[test]
fn supports_builder_params() -> zyxdb::Result<()> {
    let params = Params::new()
        .set("i32", 7_i32)?
        .set("owned", String::from("value"))?;
    let tempdir = tempfile::tempdir().unwrap();
    let db_path = tempdir.path().join("test.zyx");
    let db = Database::open(&db_path)?;

    let mut result = db.execute("RETURN $i32 AS i32, $owned AS owned", Some(&params))?;
    let row = result.next()?.expect("expected one row");
    assert_eq!(row.get_i64(0)?, 7);
    assert_eq!(row.get_str(1)?, "value");

    Ok(())
}
