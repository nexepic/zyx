"""Measure current authoritative selected Property roots before any layout change.

Uses the already-configured Release library, does not rebuild production or run
unit tests, and cleans the private object, executable and databases on failure.
Run only when other timed benchmarks have finished.
"""
import argparse
import hashlib
import itertools
import json
import os
import sys
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("buildRelease"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--library", type=Path, help="Frozen compatible libzyx_core.a; defaults to configured Release library")
    parser.add_argument("--compare-library", type=Path, help="Compatible control library; alternate control/current per process")
    parser.add_argument("--projections", type=int, nargs="+", default=[1], help="Requested field counts; 0 means all fixture fields")
    parser.add_argument("--compare-property-library", type=Path, help="Frozen compatible library: replace only its PropertyCollectionRootStore object in the current library")
    parser.add_argument("--counts", type=int, nargs="+", default=[1024, 4096])
    parser.add_argument("--fields", type=int, nargs="+", default=[1, 8, 32])
    parser.add_argument("--payloads", type=int, nargs="+", default=[96, 1024])
    parser.add_argument("--batch-only", action="store_true", help="Compare selected/full batches; omit repeated per-field point path")
    parser.add_argument("--write-cost", action="store_true", help="With --batch-only, measure fixture inserts and single-field replacements/deletes; validate snapshots outside timed writes")
    parser.add_argument("--processes", type=int, default=3, help="Independent processes per fixture (default: 3)")
    args = parser.parse_args()
    if args.write_cost and not args.batch_only:
        parser.error("--write-cost requires --batch-only")
    if args.compare_library and args.compare_property_library:
        parser.error("choose either a whole-library or a property-object control")
    if args.processes < 1:
        parser.error("--processes must be positive")
    build, output = args.build_dir.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("main.cpp")
    library = args.library.resolve() if args.library else build / "libzyx_core.a"
    libraries = {"current": library}
    if args.compare_library:
        libraries["control"] = args.compare_library.resolve()
    provenance = dict(source_sha256=sha(source), library_sha256=sha(library),
                      mode="warm; fresh snapshot; 256 owners per batch; three warmups and ten rotating samples per path; separate diagnostics",
                      dataset="p0 int64 ordinal; p1+ deterministic xorshift ASCII payload; no query execution; identical untimed fixture writes capped at 128 owners and estimated 256 KiB payload per batch",
                      limitation="selected requested prefix+missing versus same point projection and full materialization; projected values precomputed and retained for result checking; RSS includes expected-value buffers; full path checks every field and field count; this is not an alternative typed-block layout",
                      independent_processes=args.processes, library_hashes={name: sha(path) for name, path in libraries.items()},
                      rss_limit="Peak process RSS includes fixture construction and every path; unavailable on platforms without wait4; not per-query retained memory",
                      comparison_limit="Libraries may differ in multiple production changes; hashes identify the combined effect, not isolated property borrowing",
                      write_cost=args.write_cost, write_cost_limit="Single-field update uses authoritative complete-collection replacement; old snapshot retained during each write; fixture insert totals are cold construction, not per-owner steady-state timings; write counters separate; RSS includes writes when enabled",
                      requested_paths="selected/full batches" if args.batch_only else "selected batch/point/full batch",
                      commands=[], cleanup=False, status="running", planned_fixtures=len(args.counts)*len(args.fields)*len(args.payloads)*len(args.projections))
    summary, scratch = [], None
    try:
        with tempfile.TemporaryDirectory(prefix="zyx-native-property-cost-") as directory:
            scratch = Path(directory)
            if args.compare_property_library:
                frozen = args.compare_property_library.resolve()
                provenance["property_control_archive_sha256"] = sha(frozen)
                member = "PropertyCollectionRootStore.cpp.o"
                listing = subprocess.check_output(["ar", "-t", str(frozen)], text=True).splitlines()
                if listing.count(member) != 1:
                    raise RuntimeError("control archive must contain exactly one Property store object")
                extraction = ["ar", "-x", str(frozen), member]
                provenance["commands"].append(extraction)
                subprocess.run(extraction, cwd=scratch, check=True)
                control_directory = scratch / "control"
                control_directory.mkdir()
                control_library = control_directory / "libzyx_core.a"
                shutil.copy2(library, control_library)
                replacement = ["ar", "-r", str(control_library), str(scratch / member)]
                provenance["commands"].append(replacement)
                subprocess.run(replacement, check=True)
                provenance["property_control_object_sha256"] = sha(scratch / member)
                libraries["control"] = control_library
                provenance["library_hashes"] = {name: sha(path) for name, path in libraries.items()}
                provenance["comparison_limit"] = "Same current archive with only PropertyCollectionRootStore.cpp.o replaced by frozen pre-borrow object; same probe object/compiler/link flags. Compatible experimental objects, not an external ABI guarantee. All other current production changes shared."
            obj, binary = scratch / "probe.o", scratch / "probe"
            commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "zyx-compare-bench"], text=True).splitlines()
            compile_args = shlex.split(next(c for c in commands if " -c " in c and "compare_benchmark/main.cpp" in c))
            for flag in ["-MT", "-MF"]:
                index = compile_args.index(flag)
                del compile_args[index:index + 2]
            compile_args.remove("-MD")
            compile_args[compile_args.index("-o") + 1] = str(obj)
            compile_args[compile_args.index("-c") + 1] = str(source)
            provenance["commands"].append(compile_args)
            subprocess.run(compile_args, cwd=build, check=True)
            link_args = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
            link_args[link_args.index("CMakeFiles/zyx_compare_bench.dir/apps/compare_benchmark/main.cpp.o")] = str(obj)
            link_args[link_args.index("-o") + 1] = str(binary)
            library_index = next(i for i, argument in enumerate(link_args) if Path(argument).name == "libzyx_core.a")
            binaries = {}
            for version, version_library in libraries.items():
                version_binary = scratch / f"probe-{version}"
                version_link = list(link_args)
                version_link[library_index] = str(version_library)
                version_link[version_link.index("-o") + 1] = str(version_binary)
                provenance["commands"].append(version_link)
                subprocess.run(version_link, cwd=build, check=True)
                binaries[version] = version_binary
            provenance["binary_hashes"] = {name: sha(path) for name, path in binaries.items()}
            paths = ["selected_batch", "full_batch"] if args.batch_only else ["selected_batch", "selected_point", "full_batch"]
            for count, fields, payload, requested in itertools.product(args.counts, args.fields, args.payloads, args.projections):
                projected = fields if requested == 0 else requested
                if projected < 1 or projected > fields:
                    raise ValueError("projection must be between 1 and fixture field count, or 0 for all")
                observations = {version: [] for version in libraries}
                for process in range(args.processes):
                    versions = list(libraries) if process % 2 == 0 else list(reversed(libraries))
                    for version in versions:
                        database = scratch / "probe.dat"
                        command = [str(binaries[version]), str(database), str(count), str(fields), str(payload), str(projected)]
                        if args.batch_only: command.append("batch-only")
                        if args.write_cost: command.append("write-cost")
                        provenance["commands"].append(command)
                        raw = output / f"p3-property-{count}-{fields}-{payload}-projection-{projected}-{version}-process-{process}.jsonl"
                        peak_rss_bytes = None
                        with raw.open("w") as stream:
                            if hasattr(os, "wait4"):
                                child = subprocess.Popen(command, stdout=stream)
                                _, status, usage = os.wait4(child.pid, 0)
                                child.returncode = os.waitstatus_to_exitcode(status)
                                if child.returncode:
                                    raise subprocess.CalledProcessError(child.returncode, command)
                                peak_rss_bytes = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
                            else:
                                subprocess.run(command, stdout=stream, check=True)
                        if database.exists():
                            raise RuntimeError("probe left private database behind")
                        rows = [json.loads(line) for line in raw.read_text().splitlines()]
                        samples = [r for r in rows if r["event"] == "sample"]
                        if len(samples) != 10 * len(paths) or any(r["checked"] != count for r in samples):
                            raise RuntimeError("incomplete or incorrect result samples")
                        values = {path: [r["milliseconds"] for r in samples if r["path"] == path] for path in paths}
                        if any(len(v) != 10 for v in values.values()):
                            raise RuntimeError("incomplete path samples")
                        writes = [r for r in rows if r["event"] == "write_sample"]
                        if args.write_cost and (len(writes) != 20 or any(sum(r["operation"] == op for r in writes) != 10 for op in ["replace_one_field", "delete_owner"])):
                            raise RuntimeError("incomplete property write samples")
                        observations[version].append(dict(write_samples=writes, fixture_write_ms=sum(r["milliseconds"] for r in rows if r["event"] == "fixture_write"), samples_ms=values, peak_rss_bytes_including_load=peak_rss_bytes,
                            file_bytes=next(r["file_bytes"] for r in rows if r["event"] == "fixture")))
                measurements = {}
                for version, records in observations.items():
                    pooled = {path: [value for record in records for value in record["samples_ms"][path]] for path in paths}
                    measurements[version] = dict(
                        fixture_insert_total_ms=[r["fixture_write_ms"] for r in records] if args.write_cost else None,
                        write_p50_ms={op: statistics.median([w["milliseconds"] for r in records for w in r["write_samples"] if w["operation"] == op]) for op in ["replace_one_field", "delete_owner"]} if args.write_cost else None,
                        write_file_bytes=[r["write_samples"][-1]["file_after"] for r in records] if args.write_cost else None,
                        file_bytes=[r["file_bytes"] for r in records],
                        peak_rss_bytes_including_load=[r["peak_rss_bytes_including_load"] for r in records],
                        process_medians_ms=[{path: statistics.median(r["samples_ms"][path]) for path in paths} for r in records],
                        medians_ms={path: statistics.median(v) for path, v in pooled.items()},
                        p95_ms={path: statistics.quantiles(v, n=100, method="inclusive")[94] for path, v in pooled.items()})
                entry = dict(owners=count, fields=fields, payload_bytes=payload, projected_fields=projected,
                             independent_processes=args.processes, measurements=measurements)
                if "control" in measurements:
                    entry["p50_change_percent"] = {path: 100 * (measurements["current"]["medians_ms"][path] /
                        measurements["control"]["medians_ms"][path] - 1) for path in paths}
                summary.append(entry)
                (output / "p3-property-summary.json").write_text(json.dumps(summary, indent=2) + "\n")
                print(f"completed {count}/{fields}/{payload}/projection-{projected}: {measurements}", flush=True)
            if any(sha(path) != provenance["library_hashes"][version] for version, path in libraries.items()):
                raise RuntimeError("comparison library changed during run")
            if args.compare_property_library and sha(args.compare_property_library.resolve()) != provenance["property_control_archive_sha256"]:
                raise RuntimeError("frozen property control archive changed during run")
            if sha(library) != provenance["library_sha256"] or sha(source) != provenance["source_sha256"]:
                raise RuntimeError("probe source or library changed during run")
            provenance["status"] = "complete"
    finally:
        if provenance["status"] != "complete":
            provenance["status"] = "failed"
        provenance["completed_fixtures"] = len(summary)
        provenance["cleanup"] = scratch is not None and not scratch.exists()
        (output / "p3-property-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")


if __name__ == "__main__":
    main()
