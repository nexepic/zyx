use zyxdb::{params, Database, Params};

#[cfg(unix)]
use std::ffi::OsString;
#[cfg(unix)]
use std::os::unix::ffi::OsStringExt;

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
    )?;

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
    let params = Params::try_new()?
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

#[test]
fn rejects_column_index_exceeding_driver_range() -> zyxdb::Result<()> {
    let tempdir = tempfile::tempdir().unwrap();
    let db_path = tempdir.path().join("test.zyx");
    let db = Database::open(&db_path)?;
    let mut result = db.execute("RETURN 42 AS answer", None)?;

    let err = result.column_name(i32::MAX as usize + 1).unwrap_err();
    assert_eq!(err.code(), zyxdb::ErrorCode::OutOfRange);
    let row = result.next()?.expect("expected one row");
    let err = row.get_i64(i32::MAX as usize + 1).unwrap_err();
    assert_eq!(err.code(), zyxdb::ErrorCode::OutOfRange);

    Ok(())
}

#[cfg(unix)]
#[test]
fn rejects_non_utf8_database_paths() {
    let path = OsString::from_vec(vec![b't', b'e', b's', b't', 0xff]);
    let Err(err) = Database::open(path) else {
        panic!("non-UTF-8 path was accepted");
    };
    assert_eq!(err.code(), zyxdb::ErrorCode::InvalidArgument);
}
