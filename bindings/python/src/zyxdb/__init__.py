"""zyxdb — Python bindings for ZYX graph database engine."""

from importlib.metadata import PackageNotFoundError, version

from zyxdb._core import DatabaseError
from zyxdb.database import Database
from zyxdb.result import Record, Result
from zyxdb.transaction import Transaction
from zyxdb.types import Edge, Node

try:
    __version__ = version("zyxdb")
except PackageNotFoundError:
    __version__ = "0.0.0-dev"
__all__ = [
    "Database",
    "DatabaseError",
    "Edge",
    "Node",
    "Record",
    "Result",
    "Transaction",
]
