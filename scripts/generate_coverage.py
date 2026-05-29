#!/usr/bin/env python3
"""
Helper script to generate coverage reports using LLVM source-based coverage.
"""

import os
import sys
import subprocess
import argparse
import json
from pathlib import Path

IGNORE_PATTERNS = [
    "tests",
    "gtest",
    "googletest",
    "conan",
    "buildDir",
    "/v1/",
    "/usr/",
    "/opt/",
    "boost",
    "Xcode.app",
    "MSVC",
    "Program Files",
    "generated",
    "subprojects",
    "antlr4",
    "helpers/internal",  # Internal implementation files (tested through integration tests)
    "debug/",  # Debug visualization utilities (not unit-testable)
    "Terminal.cpp", # OS-level terminal wrapper (untestable without real PTY)
    "src/cli/Repl.cpp" # Top-level REPL entrypoint (blocks on stdin)
]

EXCLUDED_LINE_MARKER = "ZYX_COV_EXCL_LINE"
EXCLUDED_NEXT_MARKER = "ZYX_COV_EXCL_NEXT"
EXCLUDED_START_MARKER = "ZYX_COV_EXCL_START"
EXCLUDED_STOP_MARKER = "ZYX_COV_EXCL_STOP"
EXCLUDED_FUNCTION_MARKER = "ZYX_COV_EXCL_FUNCTION"


def find_test_binaries(build_dir):
    binaries = []
    build_path = Path(build_dir)
    for file_path in build_path.rglob("*test*"):
        if not file_path.is_file(): continue
        suffix = file_path.suffix.lower()

        # Filter source, data, layout, and config files
        # Added .pc (pkg-config) and .obj (Windows objects) to prevent llvm-cov errors
        if suffix in ['.c', '.cpp', '.h', '.hpp', '.o', '.obj', '.profraw', '.profdata',
                      '.log', '.ninja', '.xml', '.json', '.dat', '.lcov', '.txt', '.pc']: continue

        # Filter libraries (we need the linked executable for coverage mapping)
        if suffix in ['.dylib', '.so', '.dll', '.lib', '.a', '.pdb', '.exp']: continue

        # Windows specific: strict check for .exe because os.access(X_OK) is unreliable
        if os.name == 'nt' and suffix != '.exe':
            continue

        if os.access(file_path, os.X_OK):
            binaries.append(str(file_path))
    return binaries


def excluded_source_lines(source_path):
    lines = Path(source_path).read_text().splitlines()
    excluded = set()
    in_block = False
    exclude_next = False
    for line_number, line in enumerate(lines, 1):
        if in_block:
            excluded.add(line_number)
        if EXCLUDED_NEXT_MARKER in line:
            exclude_next = True
        if EXCLUDED_FUNCTION_MARKER in line:
            excluded.update(_function_lines(lines, line_number))
        if EXCLUDED_LINE_MARKER in line:
            excluded.add(line_number)
            if "catch" in line:
                excluded.update(_catch_block_lines(lines, line_number))
        if EXCLUDED_START_MARKER in line:
            excluded.add(line_number)
            in_block = True
        if EXCLUDED_STOP_MARKER in line:
            excluded.add(line_number)
            in_block = False
        elif exclude_next and EXCLUDED_NEXT_MARKER not in line:
            excluded.add(line_number)
            exclude_next = False
    return excluded


def _function_lines(lines, start_line):
    excluded = set()
    depth = 0
    saw_opening_brace = False
    for line_number in range(start_line, len(lines) + 1):
        line = lines[line_number - 1]
        excluded.add(line_number)
        depth += line.count("{")
        if "{" in line:
            saw_opening_brace = True
        depth -= line.count("}")
        if saw_opening_brace and depth <= 0:
            break
    return excluded


def _catch_block_lines(lines, start_line):
    excluded = set()
    depth = 0
    saw_opening_brace = False
    for line_number in range(start_line, len(lines) + 1):
        line = lines[line_number - 1]
        excluded.add(line_number)
        depth += line.count("{")
        if "{" in line:
            saw_opening_brace = True
        depth -= line.count("}")
        if saw_opening_brace and depth <= 0:
            break
    return excluded


def apply_source_exclusions(file_coverage, source_path):
    excluded_lines = excluded_source_lines(source_path)
    summary = file_coverage["summary"]
    if not excluded_lines:
        return {key: _normalize_metric(value) for key, value in summary.items()}
    return {
        "lines": _adjust_lines(file_coverage["segments"], summary["lines"], excluded_lines),
        "regions": _adjust_regions(file_coverage["segments"], summary["regions"], excluded_lines),
        "branches": _adjust_branches(file_coverage.get("branches", []), summary["branches"], excluded_lines),
        "functions": _adjust_functions(file_coverage["segments"], summary["functions"], source_path),
    }


def _adjust_lines(segments, raw, excluded_lines):
    executable = set()
    covered = set()
    for line, _col, count, has_count, _is_region_entry, _is_gap in segments:
        if has_count:
            executable.add(line)
            if count > 0:
                covered.add(line)
    visible = executable - excluded_lines
    return _metric(len(visible), len(covered & visible))


def _adjust_regions(segments, raw, excluded_lines):
    removed_count = 0
    removed_covered = 0
    for line, _col, count, has_count, is_region_entry, _is_gap in segments:
        if has_count and is_region_entry and line in excluded_lines:
            removed_count += 1
            if count > 0:
                removed_covered += 1
    return _metric(raw["count"] - removed_count, raw["covered"] - removed_covered)


def _adjust_branches(branches, raw, excluded_lines):
    removed_count = 0
    removed_covered = 0
    for line_start, _col_start, _line_end, _col_end, true_count, false_count, *_rest in branches:
        if line_start not in excluded_lines:
            continue
        removed_count += 2
        if true_count > 0:
            removed_covered += 1
        if false_count > 0:
            removed_covered += 1
    return _metric_with_notcovered(raw["count"] - removed_count, raw["notcovered"] - (removed_count - removed_covered))


def _adjust_functions(segments, raw, source_path):
    excluded_lines = _function_marker_lines(source_path)
    if not excluded_lines:
        return dict(raw)
    executable_function_lines = {
        line for line, _col, _count, has_count, is_region_entry, _is_gap in segments if has_count and is_region_entry
    }
    excluded = sum(1 for line in excluded_lines if line in executable_function_lines)
    excluded_covered = sum(
        1
        for line, _col, count, has_count, is_region_entry, _is_gap in segments
        if line in excluded_lines and has_count and is_region_entry and count > 0
    )
    return _metric(raw["count"] - excluded, raw["covered"] - excluded_covered)


def _function_marker_lines(source_path):
    return {
        line_number
        for line_number, line in enumerate(Path(source_path).read_text().splitlines(), 1)
        if EXCLUDED_FUNCTION_MARKER in line
    }


def _normalize_metric(raw):
    count = raw["count"]
    covered = raw["covered"]
    metric = dict(raw)
    metric["percent"] = 100.0 if count == 0 else min(100.0, covered * 100.0 / count)
    metric.setdefault("notcovered", max(0, count - covered))
    return metric


def _metric(count, covered):
    notcovered = max(0, count - covered)
    percent = 100.0 if count == 0 else min(100.0, covered * 100.0 / count)
    return {
        "count": count,
        "covered": min(covered, count),
        "notcovered": notcovered,
        "percent": percent,
    }


def _metric_with_notcovered(count, notcovered):
    notcovered = max(0, notcovered)
    return _metric(count, count - notcovered)


def coverage_export(args, main_binary, object_args, regex_pattern, profdata_file):
    cmd = [
              args.llvm_cov, "export",
              "-format=text",
              main_binary,
              f"-instr-profile={profdata_file}",
              f"-ignore-filename-regex={regex_pattern}"
          ] + object_args
    return json.loads(subprocess.check_output(cmd))


def print_adjusted_summary(coverage, source_root=None):
    headers = [
        "Filename", "Regions", "Missed Regions", "Cover",
        "Functions", "Missed Functions", "Cover",
        "Lines", "Missed Lines", "Cover",
        "Branches", "Missed Branches", "Cover"
    ]
    rows = []
    totals = {
        "regions": {"count": 0, "covered": 0},
        "functions": {"count": 0, "covered": 0},
        "lines": {"count": 0, "covered": 0},
        "branches": {"count": 0, "covered": 0},
    }
    for item in coverage["data"][0]["files"]:
        source_path = Path(item["filename"])
        metrics = item["summary"]
        if source_path.exists():
            metrics = apply_source_exclusions(item, source_path)
        for key in totals:
            totals[key]["count"] += metrics[key]["count"]
            totals[key]["covered"] += metrics[key]["covered"]
        display_name = str(source_path)
        if source_root is not None:
            try:
                display_name = str(source_path.relative_to(source_root))
            except ValueError:
                pass
        rows.append(_summary_row(display_name, metrics))
    total_metrics = {key: _metric(value["count"], value["covered"]) for key, value in totals.items()}
    rows.append(_summary_row("TOTAL", total_metrics))
    _print_table(headers, rows)


def _summary_row(filename, metrics):
    return [
        filename,
        str(metrics["regions"]["count"]),
        str(metrics["regions"]["notcovered"]),
        _format_percent(metrics["regions"]["percent"]),
        str(metrics["functions"]["count"]),
        str(metrics["functions"]["count"] - metrics["functions"]["covered"]),
        _format_percent(metrics["functions"]["percent"]),
        str(metrics["lines"]["count"]),
        str(metrics["lines"]["notcovered"]),
        _format_percent(metrics["lines"]["percent"]),
        str(metrics["branches"]["count"]),
        str(metrics["branches"]["notcovered"]),
        _format_percent(metrics["branches"]["percent"]),
    ]


def _format_percent(percent):
    return f"{percent:.2f}%"


def _print_table(headers, rows):
    widths = [len(header) for header in headers]
    for row in rows:
        for index, cell in enumerate(row):
            widths[index] = max(widths[index], len(cell))
    print("  ".join(header.ljust(widths[index]) for index, header in enumerate(headers)))
    print("  ".join("-" * width for width in widths))
    for row in rows:
        print("  ".join(cell.rjust(widths[index]) if index else cell.ljust(widths[index])
                        for index, cell in enumerate(row)))


def main():
    parser = argparse.ArgumentParser(description="Generate LLVM Coverage Reports")

    # Positional Arguments
    parser.add_argument("build_dir", help="Path to build directory")

    # Named Arguments
    parser.add_argument("--llvm-cov", required=True, help="Path to llvm-cov tool")
    parser.add_argument("--llvm-prof", required=True, help="Path to llvm-profdata tool")

    # Optional Arguments
    parser.add_argument("--html", action="store_true", help="Generate and open HTML report")
    parser.add_argument("--lcov", type=str, default=None, help="Generate LCOV report to file")
    parser.add_argument("--file", type=str, default=None, help="Show detailed source coverage for a specific file")

    args, unknown = parser.parse_known_args()

    if unknown:
        print(f"Warning: Ignoring unknown arguments: {unknown}")

    profraw_files = list(Path(args.build_dir).rglob("*.profraw"))
    profdata_file = os.path.join(args.build_dir, "code.profdata")

    if not profraw_files:
        print(f"Error: No .profraw files found in {args.build_dir}")
        sys.exit(1)

    # Step 1: Merge Profiles
    subprocess.check_call(
        [args.llvm_prof, "merge", "-sparse"] + [str(p) for p in profraw_files] + ["-o", profdata_file])

    # Step 2: Binaries
    binaries = find_test_binaries(args.build_dir)
    if not binaries:
        print("Error: No test binaries found!")
        sys.exit(1)

    main_binary = binaries[0]
    additional_binaries = binaries[1:]

    object_args = []
    for bin_path in additional_binaries:
        object_args.extend(["-object", bin_path])

    regex_pattern = ".*(" + "|".join(IGNORE_PATTERNS) + ").*"

    # Step 3: Action Dispatch
    if args.file:
        print(f"--- Coverage Detail for: {args.file} ---")
        cmd = [
                  args.llvm_cov, "show",
                  main_binary,
                  f"-instr-profile={profdata_file}",
                  f"-ignore-filename-regex={regex_pattern}",
                  "-use-color",
                  "-show-line-counts-or-regions",
                  "-show-branches=count"
              ] + object_args + [args.file]
        subprocess.call(cmd)

    elif args.html:
        output_dir = os.path.join(args.build_dir, "coverage", "html")
        print(f"--- Generating HTML Report in {output_dir} ---")
        cmd = [
                  args.llvm_cov, "show", "-format=html",
                  main_binary,
                  f"-output-dir={output_dir}",
                  f"-instr-profile={profdata_file}",
                  f"-ignore-filename-regex={regex_pattern}",
                  "-show-branches=count"
              ] + object_args
        subprocess.check_call(cmd)

        if sys.platform == "darwin":
            subprocess.call(["open", os.path.join(output_dir, "index.html")])
        print(f"Report ready: {output_dir}/index.html")

    elif args.lcov:
        print(f"--- Generating LCOV Report to {args.lcov} ---")
        cmd = [
                  args.llvm_cov, "export",
                  "-format=lcov",
                  main_binary,
                  f"-instr-profile={profdata_file}",
                  f"-ignore-filename-regex={regex_pattern}"
              ] + object_args

        with open(args.lcov, "w") as f:
            subprocess.check_call(cmd, stdout=f)
        print(f"LCOV report generated: {args.lcov}")

    else:
        print("--- Coverage Summary ---")
        coverage = coverage_export(args, main_binary, object_args, regex_pattern, profdata_file)
        print_adjusted_summary(coverage, Path.cwd())


if __name__ == "__main__":
    main()
