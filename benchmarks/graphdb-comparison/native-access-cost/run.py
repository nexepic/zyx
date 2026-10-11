"""Build the native access probe and measure scan/candidate crossover on macOS.

Uses the configured Release benchmark's compiler and libraries, never runs tests,
and removes the private executable, object and database files even on failure.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import statistics
import subprocess
import tempfile
import itertools


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=Path("buildRelease"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--kind", choices=["nodes", "edges"], default="nodes")
    parser.add_argument("--candidate-budget", action="store_true")
    parser.add_argument("--counts", type=int, nargs="+", default=[1024, 4096, 16384])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    build = args.build_dir.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmake", "--build", str(build), "--target", "zyx-compare-bench", "-j4"], check=True)
    commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands", "zyx-compare-bench"], text=True).splitlines()
    source = Path(__file__).with_name("main.cpp")
    provenance = {
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "library_sha256": hashlib.sha256((build / "libzyx_core.a").read_bytes()).hexdigest(),
        "mode": "warm; fresh snapshot per read; two warmups and ten alternating pairs",
        "scope": f"native {args.kind} access, no query execution",
        "candidate_budget": args.candidate_budget,
        "cleanup": False,
    }
    summary = []
    try:
        with tempfile.TemporaryDirectory(prefix="zyx-native-access-cost-") as directory:
            scratch = Path(directory)
            obj, binary = scratch / "probe.o", scratch / "probe"
            compile_args = shlex.split(next(c for c in commands if " -c " in c and "compare_benchmark/main.cpp" in c))
            for flag in ["-MT", "-MF"]:
                i = compile_args.index(flag)
                del compile_args[i:i + 2]
            compile_args.remove("-MD")
            compile_args[compile_args.index("-o") + 1] = str(obj)
            compile_args[compile_args.index("-c") + 1] = str(source)
            subprocess.run(compile_args, cwd=build, check=True)
            link_args = shlex.split(commands[-1].removeprefix(": && ").removesuffix(" && :"))
            i = link_args.index("CMakeFiles/zyx_compare_bench.dir/apps/compare_benchmark/main.cpp.o")
            link_args[i] = str(obj)
            link_args[link_args.index("-o") + 1] = str(binary)
            subprocess.run(link_args, cwd=build, check=True)
            percentages = [1, 10, 25, 50, 75, 100]
            cases = itertools.product(args.counts, ["clustered", "scattered"],
                                      percentages if args.kind == "edges" else [None])
            for count, distribution, selected in cases:
                database = scratch / "probe.dat"
                prefix = "p2-type-cost" if args.kind == "edges" else "p2-cost"
                suffix = f"-{selected}" if selected is not None else ""
                raw = output / f"{prefix}-{count}-{distribution}{suffix}.jsonl"
                command = [str(binary), str(database), str(count), distribution]
                if selected is not None:
                    command.extend(["edges", str(selected)])
                    if args.candidate_budget: command.append("candidate-budget")
                with raw.open("w") as stream:
                    subprocess.run(command, stdout=stream, check=True)
                if database.exists():
                    raise RuntimeError("probe left its private database behind")
                rows = [json.loads(line) for line in raw.read_text().splitlines()]
                samples = [r for r in rows if r["event"] == "sample"]
                if len(samples) != (20 if selected is not None else 120):
                    raise RuntimeError("incomplete probe samples")
                for percent in [selected] if selected is not None else percentages:
                    medians = {path: statistics.median(r["milliseconds"] for r in samples
                               if r["percent"] == percent and r["path"] == path)
                               for path in ["scan", "candidate"]}
                    summary.append(dict(nodes=count, distribution=distribution, percent=percent, **medians))
                print(f"completed {count}/{distribution}{suffix}", flush=True)
        provenance["cleanup"] = not scratch.exists()
    finally:
        prefix = "p2-type-cost" if args.kind == "edges" else "p2-cost"
        (output / f"{prefix}-calibration-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
        (output / f"{prefix}-calibration-summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
