#!/usr/bin/env python3
"""Run explicitly selected transaction OOM/retention cases in isolated processes."""
import argparse
import concurrent.futures
import json
import hashlib
import pathlib
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--crash-recovery", action="store_true")
    parser.add_argument("--property-heap-accounting", action="store_true")
    parser.add_argument("--native-stage-heap-accounting", action="store_true")
    parser.add_argument("--overlay-payload-heap-accounting", action="store_true")
    parser.add_argument("--codec-inspection", action="store_true")
    parser.add_argument("--codec-admitted-heap-accounting", action="store_true")
    parser.add_argument("--source-materialization-copy-accounting", action="store_true")
    parser.add_argument("--scoped-codec-budget", action="store_true")
    parser.add_argument("--guarded-copy-heap-accounting", action="store_true")
    parser.add_argument("--owned-std-string-heap-accounting", action="store_true")
    parser.add_argument("--owned-expression-heap-accounting", action="store_true")
    parser.add_argument("--owned-property-heap-accounting", action="store_true")
    parser.add_argument("--owned-index-definition-heap-accounting", action="store_true")
    parser.add_argument("--owned-constraint-definition-heap-accounting", action="store_true")
    parser.add_argument("--result-column-copy-admission", action="store_true")
    parser.add_argument("--query-context-account-validation", action="store_true")
    parser.add_argument("--owned-plan-heap-accounting", action="store_true")
    parser.add_argument("--owned-string-containers-heap-accounting", action="store_true")
    parser.add_argument("--crash-cycles", type=int, default=1, help="repeat crash/readonly/recovery on the same file")
    parser.add_argument("--allocation-sweep", choices=("none", "stage", "commit", "both", "overlay", "creation", "overlay-creation", "lease-reserve", "lease-cached", "lease-fresh", "leases", "mixed", "scratch-write", "distinct", "source-materialization", "normalized-patch", "plan-cache", "schema-cache", "all"), default="none")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--progress-every", type=int, default=0)
    parser.add_argument("--max-sweep-points", type=int, default=0,
                        help="zero tests every allocation in the normal operation; positive values sample evenly")
    args = parser.parse_args()
    if args.jobs < 1 or args.max_sweep_points < 0 or args.progress_every < 0 or args.crash_cycles < 1:
        parser.error("jobs must be positive and max-sweep-points nonnegative")
    worker = args.worker.resolve(strict=True)
    results = []
    sweeps = {}
    crash_recovery = None
    with tempfile.TemporaryDirectory(prefix="zyx-transaction-resource-") as temporary:
        def run(mode, fault=None, database_path=None):
            suffix = "probe" if fault is None else str(fault)
            path = database_path or pathlib.Path(temporary) / (mode + "-" + suffix + ".zyx")
            command = [str(worker), mode, str(path)]
            if fault is not None:
                command.append(str(fault))
            completed = subprocess.run(command, text=True, capture_output=True, timeout=120, check=False)
            if completed.returncode:
                raise RuntimeError(f"{mode} fault={fault} exited {completed.returncode}: {completed.stderr.strip()}")
            evidence = json.loads(completed.stdout)
            if evidence.get("mode") != mode or evidence.get("passed") is not True:
                raise RuntimeError(f"{mode} fault={fault} returned invalid evidence: {evidence!r}")
            if fault is not None and evidence.get("fault_triggered") is not True:
                raise RuntimeError(f"{mode} fault={fault} did not reach the selected allocation")
            if database_path is None:
                path.unlink(missing_ok=True)
            return evidence

        for mode in ("rollback-oom", "rollback-many-oom", "rollback-retention", "commit-retention"):
            results.append(run(mode))
        if args.property_heap_accounting:
            results.append(run("property-heap-accounting"))
        if args.native_stage_heap_accounting:
            results.append(run("native-stage-heap-accounting"))
        if args.overlay_payload_heap_accounting:
            results.append(run("overlay-payload-heap-accounting"))
        if args.codec_inspection:
            results.append(run("codec-inspection-allocation-free"))
        if args.codec_admitted_heap_accounting:
            results.append(run("codec-admitted-heap-accounting"))
        if args.owned_constraint_definition_heap_accounting:
            results.append(run("owned-constraint-definition-heap-accounting"))
        if args.owned_index_definition_heap_accounting:
            results.append(run("owned-index-definition-heap-accounting"))
        if args.query_context_account_validation:
            results.append(run("query-context-account-validation"))
        if args.result_column_copy_admission:
            results.append(run("result-column-copy-admission"))
        if args.owned_plan_heap_accounting:
            results.append(run("owned-plan-heap-accounting"))
        if args.owned_string_containers_heap_accounting:
            results.append(run("owned-string-containers-heap-accounting"))
        if args.owned_property_heap_accounting:
            results.append(run("owned-property-heap-accounting"))
        if args.owned_expression_heap_accounting:
            results.append(run("owned-expression-heap-accounting"))
        if args.owned_std_string_heap_accounting:
            results.append(run("owned-std-string-heap-accounting"))
        if args.guarded_copy_heap_accounting:
            results.append(run("guarded-copy-heap-accounting"))
        if args.source_materialization_copy_accounting:
            results.append(run("source-materialization-copy-accounting"))
        if args.scoped_codec_budget:
            results.append(run("codec-scoped-heap-accounting"))
            results.append(run("codec-scoped-partial-cancellation"))
        if args.crash_recovery:
            path = pathlib.Path(temporary) / "crashed-scratch.zyx"
            cycles = []
            for cycle in range(args.crash_cycles):
                crash_mode = "crash-query-scratch" if cycle == 0 else "crash-existing-query-scratch"
                crashed = subprocess.run([str(worker), crash_mode, str(path)],
                                         text=True, capture_output=True, timeout=120, check=False)
                if crashed.returncode != 87:
                    raise RuntimeError(f"scratch crash cycle={cycle} exited {crashed.returncode}: {crashed.stderr.strip()}")
                before = hashlib.sha256(path.read_bytes()).hexdigest()
                readonly = run("inspect-query-scratch-crash", database_path=path)
                after = hashlib.sha256(path.read_bytes()).hexdigest()
                if before != after:
                    raise RuntimeError(f"physical readonly orphan inspection cycle={cycle} changed database bytes")
                recovered = run("recover-query-scratch-crash", database_path=path)
                cycles.append({"cycle": cycle, "crash_exit": crashed.returncode, "readonly": readonly,
                               "readonly_before_sha256": before, "readonly_after_sha256": after,
                               "recovered": recovered, "file_bytes": path.stat().st_size, "passed": True})
                if args.progress_every and len(cycles) % args.progress_every == 0:
                    print(f"scratch crash recovery: {len(cycles)}/{args.crash_cycles} cycles passed", file=sys.stderr, flush=True)
            crash_recovery = {**cycles[-1], "cycles": cycles, "completed_cycles": len(cycles), "passed": True}
        for phase in ("stage", "commit", "overlay", "creation", "overlay-creation", "lease-reserve", "lease-cached", "lease-fresh", "mixed", "scratch-write", "distinct", "source-materialization", "normalized-patch", "plan-cache", "schema-cache"):
            if args.allocation_sweep not in (phase, "all") and not (
                    args.allocation_sweep == "both" and phase in ("stage", "commit")) and not (
                    args.allocation_sweep == "leases" and phase.startswith("lease-")):
                continue
            mode = phase + "-stage-allocation-fault" if phase in ("overlay", "creation", "overlay-creation", "mixed") else phase + "-allocation-fault"
            probe = run(mode)
            count = probe["allocation_attempts"]
            if not isinstance(count, int) or count < 1:
                raise RuntimeError(f"{mode} has no allocation probe")
            if args.max_sweep_points and args.max_sweep_points < count:
                selected = sorted({index * (count - 1) // max(1, args.max_sweep_points - 1)
                                   for index in range(args.max_sweep_points)})
            else:
                selected = list(range(count))
            cases = []
            executor = concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs)
            pending = {executor.submit(run, mode, fault): fault for fault in selected}
            try:
                for future in concurrent.futures.as_completed(pending):
                    cases.append(future.result())
                    if args.progress_every and len(cases) % args.progress_every == 0:
                        print(f"{mode}: {len(cases)}/{len(selected)} allocation faults passed", file=sys.stderr, flush=True)
            except BaseException:
                for future in pending:
                    future.cancel()
                executor.shutdown(wait=True, cancel_futures=True)
                raise
            else:
                executor.shutdown(wait=True)
            cases.sort(key=lambda item: item["selected_fault"])
            sweeps[phase] = {"probe": probe, "baseline_allocations": count,
                             "tested_allocations": len(cases), "exhaustive": len(cases) == count,
                             "cases": cases}
    report = {"cases": results, "allocation_sweeps": sweeps, "crash_recovery": crash_recovery,
              "passed": len(results) + sum(item["tested_allocations"] for item in sweeps.values()) + int(crash_recovery is not None),
              "failures": 0, "full_suite": False}
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
