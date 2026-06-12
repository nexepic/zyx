from __future__ import annotations

import json
from pathlib import Path

from runner import matrix


def test_run_matrix_runs_each_scale_and_execution_mode(tmp_path: Path, monkeypatch):
    calls: list[tuple[str, str, int | None]] = []

    def fake_run_benchmark(**kwargs):
        scale = kwargs["scale"]
        execution_mode = kwargs["execution_mode"]
        calls.append((scale, execution_mode, kwargs.get("threads")))
        result_dir = tmp_path / f"{scale}-{execution_mode}"
        result_dir.mkdir()
        for name in ["summary.csv", "comparison.csv", "quality_gates.json"]:
            (result_dir / name).write_text("{}\n")
        return result_dir

    monkeypatch.setattr(matrix, "run_benchmark", fake_run_benchmark)

    manifest_path = matrix.run_matrix(
        databases=["fake"],
        scales=["smoke", "small"],
        execution_modes=["cold-ish", "warm"],
        seed=42,
        output_root=tmp_path,
        warmup=0,
        iterations=1,
        threads=2,
    )

    assert calls == [
        ("smoke", "cold-ish", 2),
        ("smoke", "warm", 2),
        ("small", "cold-ish", 2),
        ("small", "warm", 2),
    ]
    manifest = json.loads(manifest_path.read_text())
    assert manifest["benchmark_matrix_schema_version"] == 1
    assert manifest["scales"] == ["smoke", "small"]
    assert manifest["execution_modes"] == ["cold-ish", "warm"]
    assert manifest["threads"] == 2
    assert len(manifest["runs"]) == 4
    assert (tmp_path / "latest-matrix.txt").read_text().strip() == manifest_path.name


def test_run_matrix_rejects_empty_dimensions(tmp_path: Path):
    try:
        matrix.run_matrix(
            databases=[],
            scales=["smoke"],
            execution_modes=["warm"],
            seed=42,
            output_root=tmp_path,
            warmup=0,
            iterations=1,
        )
    except ValueError as exc:
        assert "database" in str(exc)
    else:
        raise AssertionError("empty database list should fail")
