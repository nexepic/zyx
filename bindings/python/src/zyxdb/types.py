"""Data types for ZYX graph database results."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class Node:
    """Represents a graph node."""

    id: int
    labels: list[str]
    properties: dict[str, Any] = field(default_factory=dict)

    @property
    def label(self) -> str:
        """Primary label (first label)."""
        return self.labels[0] if self.labels else ""

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> Node:
        return cls(
            id=d["id"],
            labels=d.get("labels", [d["label"]] if "label" in d else []),
            properties=d.get("properties", {}),
        )


@dataclass
class Edge:
    """Represents a graph edge (relationship)."""

    id: int
    source_id: int
    target_id: int
    type: str
    properties: dict[str, Any] = field(default_factory=dict)

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> Edge:
        return cls(
            id=d["id"],
            source_id=d["source_id"],
            target_id=d["target_id"],
            type=d["type"],
            properties=d.get("properties", {}),
        )
