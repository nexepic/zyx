"""Check versioned and pending project code without traversing ignored build trees."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CODE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.inc', '.ipp',
                 '.js', '.jsx', '.mjs', '.cjs', '.ts', '.tsx', '.py', '.rs', '.go', '.java',
                 '.cs', '.swift', '.sh', '.cmake', '.g4'}


def main():
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=ROOT)
    failures = []
    checked = 0
    for name in sorted(set(names.decode().split('\0')) - {''}):
        path = ROOT / name
        if not path.is_file() or (path.suffix not in CODE_SUFFIXES and path.name != 'CMakeLists.txt'):
            continue
        checked += 1
        count = len(path.read_bytes().splitlines())
        if count > 3000:
            failures.append((name, count))
    for name, count in failures:
        print(f'{name}: {count} lines exceeds 3000')
    print(f'Checked {checked} code files; {len(failures)} exceed 3000 lines.')
    return bool(failures)


if __name__ == '__main__':
    sys.exit(main())
