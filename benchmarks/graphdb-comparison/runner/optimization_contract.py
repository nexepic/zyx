"""Validate the preregistered optimization experiment contract, not results.

A valid contract does not mean its workloads have been implemented or passed.
This command deliberately reports those gaps rather than accepting milestones.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path


REQUIRED_FAMILIES = frozenset({
    "point", "scan", "traversal", "aggregate", "sort", "parameters",
    "dynamic", "wide_large", "write", "mixed", "snapshot", "maintenance", "budget",
})


def validate_contract(contract: dict) -> list[str]:
    errors = []
    if contract.get("version") != 1:
        errors.append("unsupported contract version")
    resources = contract.get("resources", {})
    if not isinstance(resources, dict):
        errors.append("resources must be an object")
        resources = {}
    for key in ("container_memory_bytes", "query_materialization_bytes", "threads", "cpu_quota"):
        value = resources.get(key)
        if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
            errors.append(f"resources.{key} must be a positive integer")
    sampling = contract.get("sampling", {})
    if not isinstance(sampling, dict):
        errors.append("sampling must be an object")
        sampling = {}
    for key, minimum in (("processes", 3), ("warmups", 3), ("measurements", 10)):
        value = sampling.get(key)
        if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
            errors.append(f"sampling.{key} must be at least {minimum}")
    for key in ("aggregation", "tail_policy", "stop_rule", "cache_modes", "order"):
        if not sampling.get(key):
            errors.append(f"sampling.{key} is required")
    for key in ("targets", "protections", "correctness", "attribution", "cleanup", "datasets"):
        if not contract.get(key):
            errors.append(f"{key} is required")
    families = set()
    identifiers = set()
    workloads = contract.get("workloads", [])
    if not isinstance(workloads, list):
        return errors + ["workloads must be a list"]
    for workload in workloads:
        if not isinstance(workload, dict):
            errors.append("workload must be an object")
            continue
        identifier = workload.get("id")
        if not isinstance(identifier, str) or not identifier or identifier in identifiers:
            errors.append(f"invalid or duplicate workload id: {identifier}")
        else:
            identifiers.add(identifier)
        family = workload.get("family")
        if family not in REQUIRED_FAMILIES:
            errors.append(f"{identifier}: unknown family")
        else:
            families.add(family)
        for key in ("operation", "inputs", "oracle", "timing", "metrics"):
            if not workload.get(key):
                errors.append(f"{identifier}: {key} is required")
        if workload.get("implementation") not in {"existing", "pending"}:
            errors.append(f"{identifier}: implementation must be existing or pending")
        if workload.get("implementation") == "existing" and not workload.get("entry"):
            errors.append(f"{identifier}: existing workload requires an entry")
    missing = REQUIRED_FAMILIES - families
    if missing:
        errors.append("missing families: " + ", ".join(sorted(missing)))
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("contract", type=Path)
    args = parser.parse_args()
    try:
        contract = json.loads(args.contract.read_text())
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if not isinstance(contract, dict):
        parser.error("contract must be an object")
    errors = validate_contract(contract)
    pending = [workload["id"] for workload in contract.get("workloads", [])
               if isinstance(workload, dict) and workload.get("implementation") == "pending"]
    print(json.dumps({"contract_valid": not errors, "errors": errors,
                      "pending_workloads": pending, "acceptance_proven": False}, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
