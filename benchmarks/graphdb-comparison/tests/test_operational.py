from __future__ import annotations

import json
from pathlib import Path

from runner import operational


SUMMARY_HEADER = (
    "database,workload,scale,samples,first_ms,min_ms,avg_ms,p50_ms,p95_ms,p99_ms,"
    "max_ms,ops_per_sec,status,equivalent_mode\n"
)


def _summary_row(database: str, workload: str, p50_ms: float) -> str:
    return (
        f"{database},{workload},medium,3,{p50_ms},{p50_ms},{p50_ms},{p50_ms},"
        f"{p50_ms},{p50_ms},{p50_ms},1.0,ok,cypher\n"
    )


def _unsupported_row(database: str, workload: str) -> str:
    return f"{database},{workload},medium,0,0,0,0,0,0,0,0,0.0,unsupported,cypher\n"


def test_build_operational_rows_groups_workloads_by_database_operation(tmp_path: Path):
    scan_summary = tmp_path / "scan.csv"
    indexed_summary = tmp_path / "indexed.csv"
    multihop_summary = tmp_path / "multihop.csv"
    write_summary = tmp_path / "write.csv"
    operational_dynamic_summary = tmp_path / "operational_dynamic.csv"
    scan_summary.write_text(
        SUMMARY_HEADER
        + _summary_row("zyx", "load_nodes_edges", 100.0)
        + _summary_row("kuzu", "load_nodes_edges", 50.0)
        + _summary_row("zyx", "one_hop_expand", 1.0)
        + _summary_row("kuzu", "one_hop_expand", 5.0)
    )
    indexed_summary.write_text(
        SUMMARY_HEADER
        + _summary_row("zyx", "load_nodes_edges", 120.0)
        + _unsupported_row("kuzu", "load_nodes_edges")
        + _summary_row("zyx", "point_lookup_indexed", 0.1)
        + _summary_row("kuzu", "point_lookup_indexed", 1.0)
    )
    multihop_summary.write_text(
        SUMMARY_HEADER
        + _summary_row("zyx", "reachable_within_6", 4.0)
        + _summary_row("kuzu", "reachable_within_6", 2.0)
    )
    write_summary.write_text(
        SUMMARY_HEADER
        + _summary_row("zyx", "point_create_node", 8.0)
        + _summary_row("kuzu", "point_create_node", 4.0)
    )
    operational_dynamic_summary.write_text(
        SUMMARY_HEADER
        + _summary_row("zyx", "batch_create_edges_100", 6.0)
        + _summary_row("kuzu", "batch_create_edges_100", 3.0)
    )

    rows = operational.build_operational_rows(
        {
            "scan": scan_summary,
            "indexed": indexed_summary,
            "multihop": multihop_summary,
            "write": write_summary,
            "operational_dynamic": operational_dynamic_summary,
        },
        databases=["zyx", "kuzu"],
    )

    by_key = {(row["profile"], row["workload"]): row for row in rows}
    assert by_key[("scan", "load_nodes_edges")]["category"] == "load/index build"
    assert by_key[("scan", "load_nodes_edges")]["fastest_database"] == "kuzu"
    assert by_key[("scan", "load_nodes_edges")]["zyx_vs_fastest"] == 2.0
    assert by_key[("indexed", "load_nodes_edges")]["fastest_database"] == "zyx"
    assert by_key[("indexed", "load_nodes_edges")]["kuzu_status"] == "unsupported"
    assert by_key[("indexed", "load_nodes_edges")]["zyx_vs_fastest_non_zyx"] is None
    assert by_key[("scan", "one_hop_expand")]["category"] == "adjacency expand"
    assert by_key[("scan", "one_hop_expand")]["fastest_database"] == "zyx"
    assert by_key[("indexed", "point_lookup_indexed")]["category"] == "property filter"
    assert by_key[("multihop", "reachable_within_6")]["category"] == "adjacency expand"
    assert by_key[("write", "point_create_node")]["category"] == "point write"
    assert by_key[("operational_dynamic", "batch_create_edges_100")]["category"] == "batch write"


def test_write_operational_csv_uses_unique_primary_columns(tmp_path: Path):
    path = tmp_path / "operational.csv"
    rows = [
        {
            "category": "adjacency expand",
            "profile": "scan",
            "workload": "one_hop_expand",
            "meaning": "anchored one-hop expansion count",
            "zyx_p50_ms": 1.0,
            "kuzu_p50_ms": 2.0,
            "zyx_status": "ok",
            "kuzu_status": "ok",
            "fastest_database": "zyx",
            "fastest_p50_ms": 1.0,
            "fastest_non_primary_database": "kuzu",
            "fastest_non_primary_p50_ms": 2.0,
            "primary_database": "zyx",
            "primary_p50_ms": 1.0,
            "primary_rank": 1,
            "primary_vs_fastest": 1.0,
            "primary_vs_fastest_non_primary": 0.5,
        }
    ]

    operational.write_operational_csv(rows, ["zyx", "kuzu"], path)

    header = path.read_text().splitlines()[0].split(",")
    assert len(header) == len(set(header))
    assert "zyx_p50_ms" in header
    assert "primary_p50_ms" in header


def test_build_operational_rows_rejects_mismatched_summary_scale(tmp_path: Path):
    scan_summary = tmp_path / "scan.csv"
    scan_summary.write_text(SUMMARY_HEADER + _summary_row("zyx", "one_hop_expand", 1.0))

    try:
        operational.build_operational_rows(
            {"scan": scan_summary},
            databases=["zyx"],
            expected_scale="small",
        )
    except ValueError as exc:
        assert "expected 'small'" in str(exc)
    else:  # pragma: no cover - assertion branch for clearer failure messages
        raise AssertionError("expected mismatched scale to be rejected")


def test_run_operational_steady_state_writes_manifest_and_report(tmp_path: Path, monkeypatch):
    calls: list[tuple[str, int | None]] = []

    def fake_run_benchmark(**kwargs):
        profile = kwargs["profile"]
        calls.append((profile, kwargs.get("threads")))
        result_dir = tmp_path / profile
        result_dir.mkdir()
        rows = {
            "scan": [
                _summary_row("zyx", "load_nodes_edges", 100.0),
                _summary_row("kuzu", "load_nodes_edges", 50.0),
                _summary_row("zyx", "one_hop_expand", 1.0),
                _summary_row("kuzu", "one_hop_expand", 5.0),
            ],
            "indexed": [
                _summary_row("zyx", "load_nodes_edges", 120.0),
                _unsupported_row("kuzu", "load_nodes_edges"),
                _summary_row("zyx", "point_lookup_indexed", 0.1),
                _summary_row("kuzu", "point_lookup_indexed", 1.0),
            ],
            "multihop": [
                _summary_row("zyx", "reachable_within_6", 4.0),
                _summary_row("kuzu", "reachable_within_6", 2.0),
            ],
            "write": [
                _summary_row("zyx", "point_create_node", 8.0),
                _summary_row("kuzu", "point_create_node", 4.0),
            ],
            "operational_dynamic": [
                _summary_row("zyx", "batch_create_edges_100", 6.0),
                _summary_row("kuzu", "batch_create_edges_100", 3.0),
            ],
        }
        (result_dir / "summary.csv").write_text(SUMMARY_HEADER + "".join(rows[profile]))
        (result_dir / "comparison.csv").write_text("workload,scale\n")
        (result_dir / "quality_gates.json").write_text("{}\n")
        (result_dir / "run_status.json").write_text('{"failure_count": 0, "quality_failure_count": 0}\n')
        return result_dir

    monkeypatch.setattr(operational, "run_benchmark", fake_run_benchmark)

    manifest_path = operational.run_operational_steady_state(
        databases=["zyx", "kuzu"],
        scale="medium",
        seed=42,
        output_root=tmp_path,
        warmup=1,
        iterations=3,
        threads=4,
    )

    assert calls == [("scan", 4), ("indexed", 4), ("multihop", 4), ("write", 4), ("operational_dynamic", 4)]
    manifest = json.loads(manifest_path.read_text())
    assert manifest["operational_steady_state_schema_version"] == 1
    assert manifest["profile"] == "operational_steady_state"
    assert manifest["execution_mode"] == "warm"
    assert manifest["threads"] == 4
    assert Path(manifest["report_markdown"]).exists()
    report = Path(manifest["report_markdown"]).read_text()
    assert report.startswith("# Operational Steady-State Performance")
    assert "p50/status" in report
    assert "unsupported" in report
    assert "point write" in report
    assert "batch write" in report
    assert "| threads | 4 |" in report
    assert "Durable" not in report
    assert (tmp_path / "latest-operational-steady-state.txt").read_text().strip() == manifest_path.name
