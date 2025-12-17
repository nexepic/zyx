#!/usr/bin/env python3
"""
Helper script to generate coverage reports using LLVM source-based coverage.
- Recursively finds .profraw files in the build directory (including subdirs).
- Generates LCOV or HTML based on output argument.
"""

import os
import sys
import subprocess
import platform
from pathlib import Path

# ================= CONFIGURATION =================
# Files matching these patterns will be EXCLUDED from the report.
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
    "CLI11",
    "Xcode.app",
    "MSVC",
    "Program Files",
    "generated/"
]


def find_test_binaries(build_dir):
    """Recursively finds executable test files in the build directory."""
    binaries = []
    build_path = Path(build_dir)

    for file_path in build_path.rglob("*test*"):
        if not file_path.is_file(): continue
        suffix = file_path.suffix.lower()
        if suffix in ['.c', '.cpp', '.h', '.hpp', '.o', '.profraw', '.profdata', '.log', '.ninja', '.xml', '.json',
                      '.dat', '.lcov']: continue
        if suffix in ['.dylib', '.so', '.dll', '.lib', '.a', '.pdb', '.exp']: continue
        if platform.system() == "Windows" and suffix != '.exe': continue
        if platform.system() != "Windows" and not os.access(file_path, os.X_OK): continue
        binaries.append(str(file_path))

    return binaries


def main():
    if len(sys.argv) < 5:
        print("Usage: python generate_coverage.py <build_dir> <output_path> <llvm_cov_cmd> <llvm_profdata_cmd>")
        sys.exit(1)

    build_dir = sys.argv[1]
    output_path = sys.argv[2]
    llvm_cov_cmd = sys.argv[3]
    llvm_prof_cmd = sys.argv[4]

    # --- Step 1: Merge Profiles ---
    print(f"--- [Step 1] Merging profiles in {build_dir} ---")

    # IMPORTANT: We search recursively using rglob to find profiles in buildDir/coverage/raw/
    profraw_files = list(Path(build_dir).rglob("*.profraw"))

    # Place the merged profdata file inside buildDir (or coverage dir if preferred)
    # Keeping it in build_dir root is fine as it's a temporary build artifact
    profdata_file = os.path.join(build_dir, "code.profdata")

    if not profraw_files:
        print(f"Error: No .profraw files found in {build_dir}.")
        sys.exit(1)

    print(f"Found {len(profraw_files)} raw profile(s).")

    # Convert paths to strings
    profraw_args = [str(p) for p in profraw_files]

    # Merge command
    subprocess.check_call([llvm_prof_cmd, "merge", "-sparse"] + profraw_args + ["-o", profdata_file])

    # --- Step 2: Find Binaries ---
    print(f"--- [Step 2] Finding test binaries ---")
    binaries = find_test_binaries(build_dir)
    if not binaries:
        print("Error: No test binaries found!")
        sys.exit(1)

    object_args = []
    for bin_path in binaries:
        object_args.extend(["-object", bin_path])

    regex_pattern = ".*(" + "|".join(IGNORE_PATTERNS) + ").*"

    # --- Step 3: Generate Report (HTML or LCOV) ---
    print(f"--- [Step 3] Generating Report ---")

    # Logic: If output ends in .lcov, export LCOV. Otherwise, generate HTML dir.
    if output_path.endswith(".lcov"):
        print(f"Mode: Exporting LCOV to {output_path}")
        with open(output_path, "w") as outfile:
            cmd = [
                      llvm_cov_cmd,
                      "export",
                      "-format=lcov",
                      f"-instr-profile={profdata_file}",
                      f"-ignore-filename-regex={regex_pattern}"
                  ] + object_args
            subprocess.check_call(cmd, stdout=outfile)
    else:
        print(f"Mode: Generating HTML report in {output_path}")
        cmd = [
                  llvm_cov_cmd,
                  "show",
                  "-format=html",
                  f"-output-dir={output_path}",
                  f"-instr-profile={profdata_file}",
                  f"-ignore-filename-regex={regex_pattern}",
                  "-show-line-counts-or-regions",
                  "-show-instantiations=false"
              ] + object_args
        subprocess.check_call(cmd)

    print(f"--- Success! Output ready at {output_path} ---")


if __name__ == "__main__":
    main()
