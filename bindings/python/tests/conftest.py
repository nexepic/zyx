"""Shared fixtures for zyxdb tests."""

import shutil
import tempfile

import pytest

import zyxdb


@pytest.fixture
def db_path(tmp_path):
    """Provide a temporary directory path for a database."""
    path = tmp_path / "test_db"
    yield str(path)


@pytest.fixture
def db(db_path):
    """Provide an opened Database instance, cleaned up after test."""
    database = zyxdb.Database(db_path)
    yield database
    try:
        database.close()
    except Exception:
        pass


@pytest.fixture
def populated_db(db):
    """Provide a database with sample data."""
    db.execute("CREATE (a:Person {name: 'Alice', age: 30})")
    db.execute("CREATE (b:Person {name: 'Bob', age: 25})")
    db.execute("CREATE (c:Movie {title: 'Matrix', year: 1999})")
    db.execute(
        "MATCH (a:Person {name: 'Alice'}), (m:Movie {title: 'Matrix'}) "
        "CREATE (a)-[:WATCHED {rating: 5}]->(m)"
    )
    return db
