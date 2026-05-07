"""Pythonic Result and Record wrappers for query output."""

from __future__ import annotations

from typing import Any, Iterator


class Record:
    """A single row from a query result.

    Supports dict-like access by column name or index.
    """

    __slots__ = ("_data", "_columns")

    def __init__(self, data: dict[str, Any], columns: list[str]) -> None:
        self._data = data
        self._columns = columns

    def __getitem__(self, key: str | int) -> Any:
        if isinstance(key, int):
            return self._data[self._columns[key]]
        return self._data[key]

    def get(self, key: str, default: Any = None) -> Any:
        """Get value by column name with optional default."""
        return self._data.get(key, default)

    def keys(self) -> list[str]:
        """Return column names."""
        return list(self._columns)

    def values(self) -> list[Any]:
        """Return values in column order."""
        return [self._data[c] for c in self._columns]

    def data(self) -> dict[str, Any]:
        """Return the underlying dict."""
        return dict(self._data)

    def __repr__(self) -> str:
        items = ", ".join(f"{k}={self._data[k]!r}" for k in self._columns)
        return f"Record({items})"

    def __contains__(self, key: str) -> bool:
        """Check if a column name exists in this record."""
        return key in self._data

    def __eq__(self, other: object) -> bool:
        if isinstance(other, Record):
            return self._data == other._data
        return NotImplemented

    def __len__(self) -> int:
        return len(self._columns)


class Result:
    """Wraps a core Result object with Pythonic iteration and convenience methods.

    Supports ``for row in result`` iteration, returning :class:`Record` objects.
    """

    def __init__(self, core_result: Any) -> None:
        self._result = core_result
        self._consumed = False

    @property
    def columns(self) -> list[str]:
        """Column names from the result set."""
        return list(self._result.column_names)

    @property
    def is_success(self) -> bool:
        """Whether the query executed successfully."""
        return self._result.is_success

    @property
    def error(self) -> str | None:
        """Error message, or None if successful."""
        return self._result.error

    @property
    def duration(self) -> float:
        """Query execution duration in milliseconds."""
        return self._result.duration

    @property
    def column_names(self) -> list[str]:
        """Alias for columns (backward compatibility with core Result)."""
        return self.columns

    @property
    def column_count(self) -> int:
        """Number of columns."""
        return self._result.column_count

    def __iter__(self) -> Iterator[Record]:
        cols = self.columns
        for row_dict in self._result:
            yield Record(row_dict, cols)
        self._consumed = True

    def fetchone(self) -> Record | None:
        """Fetch the next row, or None if exhausted."""
        try:
            return next(iter(self))
        except StopIteration:
            return None

    def fetchall(self) -> list[Record]:
        """Fetch all remaining rows."""
        return list(self)

    def fetchmany(self, size: int) -> list[Record]:
        """Fetch up to *size* rows."""
        result = []
        for i, record in enumerate(self):
            if i >= size:
                break
            result.append(record)
        return result

    def single(self, strict: bool = True) -> Record:
        """Return the single result row.

        Args:
            strict: If True, raise if there are zero or more than one rows.
        """
        rows = self.fetchall()
        if strict and len(rows) != 1:
            raise ValueError(
                f"Expected exactly 1 row, got {len(rows)}"
            )
        if not rows:
            raise ValueError("Result is empty")
        return rows[0]

    def scalar(self) -> Any:
        """Return the first column of the first row."""
        record = self.single()
        return record[0]

    def data(self) -> list[dict[str, Any]]:
        """Return all rows as a list of dicts."""
        return [r.data() for r in self]

    def to_df(self) -> Any:
        """Convert to a pandas DataFrame (requires pandas)."""
        import pandas as pd  # noqa: PLC0415
        return pd.DataFrame(self.data(), columns=self.columns)

    def __repr__(self) -> str:
        status = "ok" if self.is_success else f"error={self.error!r}"
        return f"Result({status}, columns={self.columns})"
