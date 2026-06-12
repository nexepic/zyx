from __future__ import annotations

import json
from pathlib import Path

from runner import thread_scaling


SUMMARY_HEADER = (
    "database,workload,scale,samples,first_ms,min_ms,avg_ms,p50_ms,p95_ms,p99_ms,"
    "max_ms,ops_per_sec,status,equivalent_mode\n"
)


def _summary_row(database: str, workload: str, scale: str, p50_ms: float) -> str:
    return (
        f"{database},{workload},{scale},3,{p50_ms},{p50_ms},{p50_ms},{p50_ms},"
        f"{p50_ms},{p50_ms},{p50_ms},1.0,ok,cypher\n"
    )


def test_run_thread_scaling_writes_manifest_and_reports(tmp_path: Path, monkeypatch):
    calls: list[tuple[str, int]] = []

    def fake_run_benchmark(**kwargs):
        scale = kwargs["scale"]
        thread_count = kwargs["threads"]
        calls.append((scale, thread_count))
        result_dir = tmp_path / f"{scale}-threads-{thread_count}"
        result_dir.mkdir()
        zyx_p50 = 8.0 if thread_count == 1 else 4.0
        kuzu_p50 = 6.0 if thread_count == 1 else 3.0
        (result_dir / "summary.csv").write_text(
            SUMMARY_HEADER
            + _summary_row("zyx", "load_nodes_edges", scale, 100.0)
            + _summary_row("kuzu", "load_nodes_edges", scale, 50.0)
            + _summary_row("zyx", "one_hop_expand", scale, zyx_p50)
            + _summary_row("kuzu", "one_hop_expand", scale, kuzu_p50)
        )
        (result_dir / "comparison.csv").write_text("workload,scale\n")
        (result_dir / "quality_gates.json").write_text("{}\n")
        (result_dir / "run_status.json").write_text('{"failure_count": 0, "quality_failure_count": 0}\n')
        return result_dir

    monkeypatch.setattr(thread_scaling, "run_benchmark", fake_run_benchmark)

    manifest_path = thread_scaling.run_thread_scaling(
        databases=["zyx", "kuzu"],
        scales=["small"],
        thread_counts=[1, 4],
        seed=42,
        output_root=tmp_path,
        warmup=1,
        iterations=3,
        profile="scan",
        execution_mode="warm",
    )

    assert calls == [("small", 1), ("small", 4)]
    manifest = json.loads(manifest_path.read_text())
    assert manifest["thread_scaling_schema_version"] == 1
    assert manifest["thread_counts"] == [1, 4]
    assert manifest["baseline_thread_count"] == 1
    assert Path(manifest["report_csv"]).exists()
    assert Path(manifest["report_markdown"]).exists()
    assert (tmp_path / "latest-thread-scaling.txt").read_text().strip() == manifest_path.name

    report = Path(manifest["report_markdown"]).read_text()
    assert report.startswith("# Thread Scaling Performance")
    assert "| threads | 1, 4 |" in report
    assert "`one_hop_expand`" in report
    assert "zyx speedup" in report
    assert "2.00" in report

    csv_text = Path(manifest["report_csv"]).read_text()
    assert "speedup_vs_baseline" in csv_text
    assert "one_hop_expand,zyx,4,ok,4.0,1,8.0,2.0" in csv_text


def test_run_thread_scaling_rejects_invalid_dimensions(tmp_path: Path):
    try:
        thread_scaling.run_thread_scaling(
            databases=["zyx"],
            scales=["small"],
            thread_counts=[],
            seed=42,
            output_root=tmp_path,
            warmup=0,
            iterations=1,
        )
    except ValueError as exc:
        assert "thread count" in str(exc)
    else:
        raise AssertionError("empty thread-count list should fail")

    try:
        thread_scaling.run_thread_scaling(
            databases=["zyx"],
            scales=["small"],
            thread_counts=[-1],
            seed=42,
            output_root=tmp_path,
            warmup=0,
            iterations=1,
        )
    except ValueError as exc:
        assert ">= 0" in str(exc)
    else:
        raise AssertionError("negative thread count should fail")
