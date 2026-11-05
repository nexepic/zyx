"""Compare a gtest discovery listing with this run's actual XML inventory."""
import argparse
import collections
import json
from pathlib import Path
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument("--discovery", type=Path, required=True)
parser.add_argument("--xml-directory", type=Path, required=True)
parser.add_argument("--source", required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
names, suite = [], ""
for line in args.discovery.read_text().splitlines():
    value = line.split("#", 1)[0].strip()
    if not value:
        continue
    if not line.startswith(" "):
        suite = value
    else:
        names.append(suite + value)
disabled = {n for n in names if any(p.startswith("DISABLED_") for p in n.split("."))}
enabled = set(names) - disabled
executed, failures, skips = collections.Counter(), [], []
files = sorted(args.xml_directory.glob("*.xml"))
for path in files:
    for case in ET.parse(path).iter("testcase"):
        name = case.get("classname") + "." + case.get("name")
        if case.get("status") == "run":
            executed[name] += 1
        if case.find("failure") is not None:
            failures.append(name)
        if case.find("skipped") is not None:
            skips.append(name)
result = dict(
    source=args.source, xml_files=len(files),
    discovered_enabled=len(enabled), disabled=len(disabled),
    executed_distinct=len(executed), executed_total=sum(executed.values()),
    discovery_duplicates=[n for n, count in collections.Counter(names).items() if count != 1],
    missing=sorted(enabled - executed.keys()), extra=sorted(executed.keys() - enabled),
    duplicates=sorted(n for n, count in executed.items() if count != 1),
    failures=failures, skips=skips, disabled_names=sorted(disabled),
)
args.output.write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps({k: v for k, v in result.items() if k != "disabled_names"}, indent=2))
raise SystemExit(0 if files and not any(result[k] for k in (
    "discovery_duplicates", "missing", "extra", "duplicates", "failures", "skips")) else 1)
