import json
import os
import subprocess
import sys
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
    "helpers/internal",
    "debug/",
    "Terminal.cpp",
    "src/cli/Repl.cpp",
]


EXCLUDED_LINE_MARKER = "ZYX_COV_EXCL_LINE"
EXCLUDED_NEXT_MARKER = "ZYX_COV_EXCL_NEXT"
EXCLUDED_START_MARKER = "ZYX_COV_EXCL_START"
EXCLUDED_STOP_MARKER = "ZYX_COV_EXCL_STOP"


def test_source_exclusion_adjustment_preserves_files_without_markers(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "NoMarkers.cpp"
    source_file.write_text("int covered() { return 1; }\nint missed() { return 0; }\n")
    file_coverage = {
        "segments": [[1, 1, 1, True, True, False]],
        "branches": [],
        "summary": {
            "lines": {"count": 2, "covered": 1, "percent": 50.0},
            "regions": {"count": 2, "covered": 1, "percent": 50.0},
            "branches": {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0},
            "functions": {"count": 2, "covered": 1, "percent": 50.0},
        },
    }

    assert generate_coverage.apply_source_exclusions(file_coverage, source_file) == {
        "lines": {"count": 2, "covered": 1, "notcovered": 1, "percent": 50.0},
        "regions": {"count": 2, "covered": 1, "notcovered": 1, "percent": 50.0},
        "branches": {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0},
        "functions": {"count": 2, "covered": 1, "notcovered": 1, "percent": 50.0},
    }


def test_source_exclusion_adjustment_honors_multiline_branch_marker(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "MultilineBranch.cpp"
    source_file.write_text(
        "bool guard(bool a, bool b) {\n"
        "  if (a ||\n"
        "      b) { // ZYX_COV_EXCL_LINE\n"
        "    return true;\n"
        "  }\n"
        "  return false;\n"
        "}\n"
    )
    file_coverage = {
        "segments": [
            [1, 1, 1, True, True, False],
            [2, 3, 1, True, True, False],
            [3, 7, 0, True, True, False],
        ],
        "branches": [
            [2, 7, 3, 8, 1, 0],
        ],
        "summary": {
            "lines": {"count": 7, "covered": 6, "percent": 85.71},
            "regions": {"count": 3, "covered": 2, "percent": 66.67},
            "branches": {"count": 2, "covered": 1, "notcovered": 1, "percent": 50.0},
            "functions": {"count": 1, "covered": 1, "percent": 100.0},
        },
    }

    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert adjusted["branches"] == {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0}
    assert adjusted["lines"]["count"] == 1
    assert adjusted["regions"]["count"] == 1


def test_source_exclusion_adjustment_treats_continued_marker_as_statement_prefix(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "ContinuedMarker.cpp"
    source_file.write_text(
        "bool guard(bool a, bool b, bool c) {\n"
        "  if (a ||\n"
        "      b ||\n"
        "      c) { // ZYX_COV_EXCL_LINE\n"
        "    return true;\n"
        "  }\n"
        "  return false;\n"
        "}\n"
    )
    file_coverage = {
        "segments": [
            [1, 1, 1, True, True, False],
            [2, 3, 1, True, True, False],
            [3, 7, 1, True, True, False],
            [4, 7, 0, True, True, False],
        ],
        "branches": [
            [2, 7, 2, 11, 1, 0],
            [3, 7, 4, 8, 0, 1],
        ],
        "summary": {
            "lines": {"count": 8, "covered": 7, "percent": 87.5},
            "regions": {"count": 4, "covered": 3, "percent": 75.0},
            "branches": {"count": 4, "covered": 2, "notcovered": 2, "percent": 50.0},
            "functions": {"count": 1, "covered": 1, "percent": 100.0},
        },
    }

    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert adjusted["branches"] == {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0}
    assert adjusted["lines"]["count"] == 1


def test_driver_abi_coverage_summary_honors_source_exclusion_markers(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    build_dir = repo_root / "buildDir"
    source_file = repo_root / "src/api/driver_abi/DriverAbiValueRefs.cpp"
    profdata_file = build_dir / "code.profdata"

    if not profdata_file.exists():
        raise AssertionError("buildDir/code.profdata is required; run ./scripts/run_tests.sh --quick first")

    coverage = _llvm_cov_export(repo_root, build_dir, profdata_file)
    file_coverage = next(
        file for file in coverage["data"][0]["files"] if Path(file["filename"]).resolve() == source_file
    )
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    raw = file_coverage["summary"]
    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert adjusted["lines"]["count"] < raw["lines"]["count"]
    assert adjusted["regions"]["count"] < raw["regions"]["count"]
    assert adjusted["branches"]["count"] < raw["branches"]["count"]
    assert adjusted["lines"]["notcovered"] <= raw["lines"]["count"] - raw["lines"]["covered"]
    assert adjusted["regions"]["notcovered"] <= raw["regions"]["notcovered"]
    assert adjusted["branches"]["notcovered"] <= raw["branches"]["notcovered"]
    assert adjusted["functions"]["percent"] >= 95.0


def _llvm_cov_export(repo_root: Path, build_dir: Path, profdata_file: Path):
    binaries = _find_test_binaries(build_dir)
    if not binaries:
        raise AssertionError("No test binaries found in buildDir")

    llvm_cov = _tool_path("llvm-cov")
    regex_pattern = ".*(" + "|".join(IGNORE_PATTERNS) + ").*"
    cmd = [
        llvm_cov,
        "export",
        "-format=text",
        binaries[0],
        f"-instr-profile={profdata_file}",
        f"-ignore-filename-regex={regex_pattern}",
    ]
    for binary in binaries[1:]:
        cmd.extend(["-object", binary])

    result = subprocess.run(cmd, cwd=repo_root, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return json.loads(result.stdout)


def _tool_path(name: str) -> str:
    direct = _run_tool_lookup([name])
    if direct is not None:
        return direct
    xcrun = _run_tool_lookup(["xcrun", "--find", name])
    if xcrun is not None:
        return xcrun
    raise AssertionError(f"Unable to locate {name}")


def _run_tool_lookup(cmd):
    try:
        result = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    except FileNotFoundError:
        return None
    if result.returncode != 0:
        return None
    path = result.stdout.strip()
    return path or None


def _find_test_binaries(build_dir: Path):
    binaries = []
    for file_path in build_dir.rglob("*test*"):
        if not file_path.is_file():
            continue
        suffix = file_path.suffix.lower()
        if suffix in {
            ".c",
            ".cpp",
            ".h",
            ".hpp",
            ".o",
            ".obj",
            ".profraw",
            ".profdata",
            ".log",
            ".ninja",
            ".xml",
            ".json",
            ".dat",
            ".lcov",
            ".txt",
            ".pc",
        }:
            continue
        if suffix in {".dylib", ".so", ".dll", ".lib", ".a", ".pdb", ".exp"}:
            continue
        if os.name == "nt" and suffix != ".exe":
            continue
        if os.access(file_path, os.X_OK):
            binaries.append(str(file_path))
    return binaries
