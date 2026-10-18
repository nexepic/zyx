"""Select bounded read supplements under the registered P50/P95 rules."""
from __future__ import annotations

import math
from statistics import median


def select_read_supplements(report: dict, primary: set[str]) -> tuple[dict, list]:
    selected: dict = {}
    reasons = []
    for row in report["comparisons"]:
        if min(row["control_processes"], row["current_processes"]) != 3:
            continue
        processes = [p for p in report["processes"]
                     if p["scale"] == row["scale"] and p["workload"] == row["workload"]]
        profile_names = {p["profile"] for p in processes}
        if len(profile_names) != 1:
            raise ValueError("read comparison must identify one profile")
        profile = next(iter(profile_names))
        tails = {}
        for engine in ("control", "current"):
            values = [p["p95_ms"] for p in processes if p["engine"] == engine]
            if len(values) != 3 or any(not math.isfinite(v) or v <= 0 for v in values):
                raise ValueError("read supplement requires three valid P95 observations per source")
            tails[engine] = values
        control = row["control_median_process_p50_ms"]
        current = row["current_median_process_p50_ms"]
        if any(not math.isfinite(v) or v <= 0 for v in (control, current)):
            raise ValueError("read supplement requires positive finite P50 observations")
        variation = (row["control_process_p50_max_ms"] - row["control_process_p50_min_ms"]) / control
        near = (abs((1 - current / control) - .10) <= .05 if row["workload"] in primary
                else abs((current / control - 1) - .05) <= .05)
        tail_ranges = {engine: (max(values) - min(values)) / median(values)
                       for engine, values in tails.items()}
        tail_change = median(tails["current"]) / median(tails["control"]) - 1
        near_tail = abs(tail_change - .05) <= .05
        unstable_tail = max(tail_ranges.values()) > .05
        if variation > .05 or near or near_tail or unstable_tail:
            selected.setdefault((row["scale"], profile), []).append((row["workload"], control * 103 / 1000))
            reasons.append({"scale": row["scale"], "profile": profile, "workload": row["workload"],
                            "control_p50_range_fraction": variation, "near_gate": near,
                            "p95_change_fraction": tail_change, "near_p95_gate": near_tail,
                            "control_p95_range_fraction": tail_ranges["control"],
                            "current_p95_range_fraction": tail_ranges["current"],
                            "unstable_tail": unstable_tail})
    return selected, reasons


def select_write_supplements(report: dict) -> list:
    """Apply the same protection-tail rule to restored-baseline writes."""
    rows, processes = [], []
    for row in report["rows"]:
        engines = row["processes"]
        if any(len(engines.get(engine, [])) != 3 for engine in ("control", "current")):
            continue
        control = [p["p50_ms"] for p in engines["control"]]
        current = [p["p50_ms"] for p in engines["current"]]
        if any(not math.isfinite(v) or v <= 0 for v in control + current):
            raise ValueError("write supplement requires positive finite P50 observations")
        rows.append({"scale": "write", "workload": row["workload"],
                     "control_processes": 3, "current_processes": 3,
                     "control_median_process_p50_ms": median(control),
                     "current_median_process_p50_ms": median(current),
                     "control_process_p50_min_ms": min(control),
                     "control_process_p50_max_ms": max(control)})
        for engine in ("control", "current"):
            processes.extend({"scale": "write", "profile": "write",
                              "workload": row["workload"], "engine": engine,
                              "p95_ms": p["p95_ms"]} for p in engines[engine])
    return select_read_supplements({"comparisons": rows, "processes": processes}, set())[1]
