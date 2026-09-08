import os
import re
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
]


EXCLUDED_LINE_MARKER = "ZYX_COV_EXCL_LINE"
EXCLUDED_NEXT_MARKER = "ZYX_COV_EXCL_NEXT"
EXCLUDED_START_MARKER = "ZYX_COV_EXCL_START"
EXCLUDED_STOP_MARKER = "ZYX_COV_EXCL_STOP"


def test_default_ignore_scope_keeps_native_implementation_directories(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    ignore_regex = re.compile(".*(" + "|".join(generate_coverage.IGNORE_PATTERNS) + ").*")
    for native_source in (
        "src/query/parser/cypher/helpers/internal/ExpressionBuilder.cpp",
        "src/debug/PerfTrace.cpp",
        "src/cli/Repl.cpp",
    ):
        assert ignore_regex.fullmatch(native_source) is None


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


def test_declaration_only_headers_are_reported_as_non_executable(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "Interface.hpp"
    source_file.write_text(
        "class Interface {\n"
        "public:\n"
        "  virtual ~Interface() = default;\n"
        "  virtual void check() const = 0;\n"
        "};\n"
    )
    file_coverage = {
        "segments": [[3, 3, 0, True, True, False]],
        "branches": [],
        "summary": {
            "lines": {"count": 1, "covered": 0, "percent": 0.0},
            "regions": {"count": 1, "covered": 0, "percent": 0.0},
            "branches": {"count": 0, "covered": 0, "notcovered": 0, "percent": 0.0},
            "functions": {"count": 1, "covered": 0, "percent": 0.0},
        },
    }

    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)
    assert all(metric == {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0}
               for metric in adjusted.values())


def test_find_test_binaries_ignores_executable_scripts(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    build_dir = tmp_path / "build"
    build_dir.mkdir()
    native_name = "zyx_test_suite.exe" if os.name == "nt" else "zyx_test_suite"
    native_test = build_dir / native_name
    # A minimal native image prefix is sufficient for discovery; llvm-cov is
    # not invoked by this unit test.
    native_test.write_bytes(b"\xcf\xfa\xed\xfe" + b"\x00" * 16)
    python_bin = build_dir / "python-bindings-venv" / "bin"
    python_bin.mkdir(parents=True)
    script_name = "pytest.exe" if os.name == "nt" else "pytest"
    python_test_script = python_bin / script_name
    python_test_script.write_text("#!/usr/bin/env python3\n")
    native_test.chmod(0o755)
    python_test_script.chmod(0o755)

    assert generate_coverage.find_test_binaries(build_dir) == [str(native_test)]


def test_find_coverage_profiles_skips_empty_crash_child_profiles(tmp_path, capsys):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    profile_dir = tmp_path / "profiles"
    profile_dir.mkdir()
    completed_profile = profile_dir / "completed.profraw"
    completed_profile.write_bytes(b"llvm-profile-data")
    earlier_completed_profile = profile_dir / "another-completed.profraw"
    earlier_completed_profile.write_bytes(b"more-llvm-profile-data")
    interrupted_profile = profile_dir / "interrupted-child.profraw"
    interrupted_profile.touch()

    assert generate_coverage.find_coverage_profiles(profile_dir) == [earlier_completed_profile, completed_profile]
    assert "Skipping empty LLVM coverage profile" in capsys.readouterr().err


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


def test_source_exclusion_does_not_extend_past_a_terminated_marker_line(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "TerminatedMarker.cpp"
    source_file.write_text(
        "int value() {\n"
        "  return 1; // ZYX_COV_EXCL_LINE\n"
        "}\n"
        "bool still_measurable(bool value) {\n"
        "  return value;\n"
        "}\n"
    )

    assert generate_coverage.excluded_source_lines(source_file) == {2}


def test_branch_adjustment_ignores_folded_zero_count_branches(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "FoldedBranch.cpp"
    source_file.write_text("bool guard(bool value) { return value; }\n")
    file_coverage = {
        "segments": [[1, 1, 1, True, True, False]],
        "branches": [
            [1, 12, 1, 17, 3, 2],
            [1, 24, 1, 30, 0, 0, 0, 0, 4],
        ],
        "summary": {
            "lines": {"count": 1, "covered": 1, "percent": 100.0},
            "regions": {"count": 1, "covered": 1, "percent": 100.0},
            "branches": {"count": 2, "covered": 2, "notcovered": 0, "percent": 100.0},
            "functions": {"count": 1, "covered": 1, "percent": 100.0},
        },
    }

    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert adjusted["branches"] == {"count": 2, "covered": 2, "notcovered": 0, "percent": 100.0}


def test_region_adjustment_aggregates_template_instantiations_by_source_site(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "TemplatedHeader.hpp"
    source_file.write_text(
        "template <typename T>\n"
        "int value(T v) { return v ? 1 : 0; }\n"
        "int defensive() { return 0; } // ZYX_COV_EXCL_LINE\n"
    )
    file_coverage = {
        "segments": [
            [2, 1, 4, True, True, False],
            [2, 1, 0, True, True, False],
            [2, 16, 0, True, True, False],
            [2, 16, 3, True, True, False],
            [3, 1, 0, True, True, False],
        ],
        "branches": [],
        "summary": {
            "lines": {"count": 2, "covered": 1, "percent": 50.0},
            "regions": {"count": 5, "covered": 2, "percent": 40.0},
            "branches": {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0},
            "functions": {"count": 2, "covered": 1, "percent": 50.0},
        },
    }

    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert adjusted["regions"] == {"count": 2, "covered": 2, "notcovered": 0, "percent": 100.0}


def test_header_line_adjustment_aggregates_template_instantiations_without_markers(tmp_path):
    repo_root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    source_file = tmp_path / "TemplatedHeader.hpp"
    source_file.write_text(
        "template <typename T>\n"
        "int value(T value) { return value ? 1 : 0; }\n"
    )
    file_coverage = {
        "segments": [
            [2, 1, 4, True, True, False],
            [2, 1, 0, True, True, False],
            [2, 30, 0, True, True, False],
            [2, 30, 3, True, True, False],
        ],
        "branches": [],
        "summary": {
            "lines": {"count": 2, "covered": 1, "percent": 50.0},
            "regions": {"count": 4, "covered": 2, "percent": 50.0},
            "branches": {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0},
            "functions": {"count": 2, "covered": 2, "percent": 100.0},
        },
    }

    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert adjusted["lines"] == {"count": 1, "covered": 1, "notcovered": 0, "percent": 100.0}
    assert adjusted["regions"] == {"count": 2, "covered": 2, "notcovered": 0, "percent": 100.0}


def test_driver_abi_source_exclusion_markers_adjust_coverage_deterministically():
    repo_root = Path(__file__).resolve().parents[1]
    source_file = repo_root / "src/api/driver_abi/DriverAbiValueRefs.cpp"
    sys.path.insert(0, str(repo_root / "scripts"))
    import generate_coverage

    # This test intentionally does not depend on buildDir/code.profdata: a
    # targeted test run legitimately replaces that artifact with a partial
    # profile.  The synthetic counters isolate the marker policy itself.
    file_coverage = {
        "segments": [
            [15, 1, 1, True, True, False],
            [16, 1, 0, True, True, False],
            [17, 1, 1, True, True, False],
        ],
        "branches": [[16, 1, 16, 20, 0, 1]],
        "summary": {
            "lines": {"count": 3, "covered": 2, "percent": 66.67},
            "regions": {"count": 3, "covered": 2, "percent": 66.67},
            "branches": {"count": 2, "covered": 1, "notcovered": 1, "percent": 50.0},
            "functions": {"count": 1, "covered": 1, "percent": 100.0},
        },
    }
    adjusted = generate_coverage.apply_source_exclusions(file_coverage, source_file)

    assert 16 in generate_coverage.excluded_source_lines(source_file)
    assert adjusted["lines"] == {"count": 2, "covered": 2, "notcovered": 0, "percent": 100.0}
    assert adjusted["regions"] == {"count": 2, "covered": 2, "notcovered": 0, "percent": 100.0}
    assert adjusted["branches"] == {"count": 0, "covered": 0, "notcovered": 0, "percent": 100.0}
