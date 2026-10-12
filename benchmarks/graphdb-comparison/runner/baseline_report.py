"""Summarize independent baseline rounds without pooling process samples."""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from statistics import median


def summarize(root: Path) -> dict:
    grouped: dict[tuple[str, str, str], list[dict]] = {}
    for file in sorted(root.glob("*-round-*/*/summary.csv")):
        scale, round_id = file.parent.parent.name.rsplit("-round-", 1)
        with file.open() as stream:
            for row in csv.DictReader(stream):
                key = (scale, row["workload"], row["database"])
                grouped.setdefault(key, []).append({"round": int(round_id), **row})
    rows = []
    for (scale, workload, database), samples in sorted(grouped.items()):
        valid = [row for row in samples if row["status"] == "ok"]
        record = {"scale": scale, "workload": workload, "database": database,
                  "processes": len(valid), "statuses": [row["status"] for row in samples], "rounds": samples}
        record["three_rounds_complete"] = (len(samples) == 3 and {row["round"] for row in samples} == {1, 2, 3}
                                            and all(row["status"] == "unsupported" or
                                                    (row["status"] == "ok" and int(row["samples"]) == 10)
                                                    for row in samples))
        if valid:
            medians = [float(row["p50_ms"]) for row in valid]
            record.update(p50_ms=median(medians), process_p50_min_ms=min(medians),
                          process_p50_max_ms=max(medians),
                          median_process_p95_ms=median(float(row["p95_ms"]) for row in valid))
        rows.append(record)
    payload = {"aggregation": "median of independent process P50s; P95 remains descriptive",
               "rows": rows}
    (root / "baseline-summary.json").write_text(json.dumps(payload, indent=2) + "\n")
    lines = ["# Docker read baseline", "", "Median of process P50s, in milliseconds. See JSON for individual rounds and process variation.", "",
             "| Scale | Workload | ZYX P50 | Kuzu P50 | ZYX/Kuzu |", "|---|---|---:|---:|---:|"]
    indexed = {(row["scale"], row["workload"], row["database"]): row for row in rows}
    for scale, workload in sorted({(row["scale"], row["workload"]) for row in rows}):
        zyx = indexed.get((scale, workload, "zyx"), {}).get("p50_ms")
        kuzu = indexed.get((scale, workload, "kuzu"), {}).get("p50_ms")
        display = lambda value: "N/A" if value is None else f"{value:.6f}"
        ratio = zyx / kuzu if zyx is not None and kuzu else None
        lines.append(f"| {scale} | {workload} | {display(zyx)} | {display(kuzu)} | {display(ratio)} |")
    (root / "baseline-summary.md").write_text("\n".join(lines) + "\n")
    return payload


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    summarize(parser.parse_args().root)
