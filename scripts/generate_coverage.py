#!/usr/bin/env python3
import os
import sys
import glob
import subprocess
import platform
from pathlib import Path

# ================= CONFIGURATION =================
# Add keywords here to exclude files from the report.
# These will be compiled into a regex: .*(item1|item2).*
IGNORE_PATTERNS = [
    "tests",  # Exclude test source code
    "gtest",  # Exclude GoogleTest
    "googletest",
    "conan",  # Exclude Conan packages
    "buildDir",  # Exclude build artifacts
    "/v1/",  # Exclude C++ standard library (Linux/Mac)
    "/usr/",  # Exclude system headers
    "/opt/",  # Exclude Homebrew/opt headers
    "boost",  # Exclude Boost
    "CLI11",  # Exclude CLI11
    "Xcode.app",  # Exclude macOS SDK
    "MSVC",  # Exclude Windows MSVC headers
    "Program Files"  # Exclude Windows system files
]


def find_test_binaries(build_dir):
    """Recursively finds executable test files in the build directory."""
    binaries = []
    build_path = Path(build_dir)

    # Recursively look for files containing "test" in their name
    for file_path in build_path.rglob("*test*"):
        if not file_path.is_file():
            continue

        suffix = file_path.suffix.lower()

        # Exclude common non-executable extensions
        if suffix in ['.c', '.cpp', '.h', '.hpp', '.o', '.obj', '.profraw', '.profdata', '.log', '.ninja', '.xml',
                      '.json']:
            continue
        # Exclude library files
        if suffix in ['.dylib', '.so', '.dll', '.lib', '.a', '.pdb', '.exp', '.ilk']:
            continue

        # On Windows, strictly require .exe
        if platform.system() == "Windows" and suffix != '.exe':
            continue

        # On Unix, ensure the file is executable
        if platform.system() != "Windows" and not os.access(file_path, os.X_OK):
            continue

        binaries.append(str(file_path))

    return binaries


def main():
    # Expect 4 arguments: build_dir, output_file, llvm-cov cmd, llvm-profdata cmd
    if len(sys.argv) < 5:
        print("Usage: python generate_coverage.py <build_dir> <output_file> <llvm_cov_cmd> <llvm_profdata_cmd>")
        sys.exit(1)

    build_dir = sys.argv[1]
    output_file = sys.argv[2]
    llvm_cov_cmd = sys.argv[3]
    llvm_prof_cmd = sys.argv[4]

    print(f"--- [Step 1] Merging profiles in {build_dir} ---")
    # Find all .profraw files
    profraw_files = list(glob.glob(os.path.join(build_dir, "*.profraw")))
    profdata_file = os.path.join(build_dir, "code.profdata")

    if not profraw_files:
        print(f"Error: No .profraw files found in {build_dir}. Did tests run?")
        sys.exit(1)

    # Command: llvm-profdata merge -sparse file1 file2 ... -o code.profdata
    subprocess.check_call([llvm_prof_cmd, "merge", "-sparse"] + profraw_files + ["-o", profdata_file])

    print(f"--- [Step 2] Finding test binaries ---")
    binaries = find_test_binaries(build_dir)
    if not binaries:
        print("Error: No test binaries found! Check your build output.")
        sys.exit(1)

    print(f"Found {len(binaries)} test binaries: {binaries}")

    print(f"--- [Step 3] Generating LCOV report ---")

    # Prepare arguments for -object flag
    object_args = []
    for bin_path in binaries:
        object_args.extend(["-object", bin_path])

    # Construct the ignore regex
    regex_pattern = ".*(" + "|".join(IGNORE_PATTERNS) + ").*"
    print(f"Using ignore filter: {regex_pattern}")

    # Command: llvm-cov export -format=lcov -instr-profile=... -ignore-filename-regex=... -object bin1 ...
    with open(output_file, "w") as outfile:
        cmd = [
                  llvm_cov_cmd,
                  "export",
                  "-format=lcov",
                  f"-instr-profile={profdata_file}",
                  f"-ignore-filename-regex={regex_pattern}"
              ] + object_args

        # Execute and pipe output directly to file
        subprocess.check_call(cmd, stdout=outfile)

    print(f"--- Success! Report generated at {output_file} ---")


if __name__ == "__main__":
    main()
