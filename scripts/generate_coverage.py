#!/usr/bin/env python3
"""
Helper script to generate coverage reports using LLVM source-based coverage.
"""

import os
import sys
import subprocess
import argparse
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
    "generated/",
    "linenoise.hpp",
    "subprojects/",
    "antlr4"
]


def find_test_binaries(build_dir):
    binaries = []
    build_path = Path(build_dir)
    for file_path in build_path.rglob("*test*"):
        if not file_path.is_file(): continue
        suffix = file_path.suffix.lower()
        if suffix in ['.c', '.cpp', '.h', '.hpp', '.o', '.profraw', '.profdata',
                      '.log', '.ninja', '.xml', '.json', '.dat', '.lcov', '.txt']: continue
        if suffix in ['.dylib', '.so', '.dll', '.lib', '.a', '.pdb', '.exp']: continue
        if os.access(file_path, os.X_OK):
            binaries.append(str(file_path))
    return binaries


def main():
    parser = argparse.ArgumentParser(description="Generate LLVM Coverage Reports")

    # Positional Arguments
    parser.add_argument("build_dir", help="Path to build directory")

    # Named Arguments (Changed from positional to avoid alignment errors)
    parser.add_argument("--llvm-cov", required=True, help="Path to llvm-cov tool")
    parser.add_argument("--llvm-prof", required=True, help="Path to llvm-profdata tool")

    # Optional Arguments
    parser.add_argument("--html", action="store_true", help="Generate and open HTML report")
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

    else:
        print("--- Coverage Summary ---")
        cmd = [
                  args.llvm_cov, "report",
                  main_binary,
                  f"-instr-profile={profdata_file}",
                  f"-ignore-filename-regex={regex_pattern}",
                  "-use-color"
              ] + object_args
        subprocess.call(cmd)


if __name__ == "__main__":
    main()
