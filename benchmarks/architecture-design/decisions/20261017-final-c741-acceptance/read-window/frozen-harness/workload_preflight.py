"""Execute untimed native workload correctness traces with independent full-result oracles.

Smoke exercises every family with bounded data. Contract mode generates the registered
sizes; neither mode records latency or establishes baseline/performance acceptance.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, TimeoutError
import hashlib
import json
import os
from pathlib import Path
import random
import re
import subprocess
import sys
import tempfile
import time

from runner.optimization_contract import REQUIRED_FAMILIES




NATIVE_FAMILY_ORDER = ("point", "scan", "traversal", "aggregate", "sort", "parameters", "dynamic", "wide_large", "write", "mixed", "snapshot", "maintenance", "budget")
KUZU_FAMILY_ORDER = tuple(name for name in NATIVE_FAMILY_ORDER if name not in ("dynamic", "budget"))
_active_family_progress = None


class ContractFamilyProgress(list):
    """Enforce the registered 30-minute stop per workload family, not per bundle."""
    def __init__(self, output: Path, names=NATIVE_FAMILY_ORDER, clock=time.monotonic):
        super().__init__()
        self.output, self.names, self.clock = output, names, clock
        self.started = clock()

    @property
    def current_family(self):
        return self.names[len(self)] if len(self) < len(self.names) else "completed"

    def remaining(self):
        remaining = 1800 - (self.clock() - self.started)
        if remaining <= 0:
            raise TimeoutError(f"contract family {self.current_family} exceeded its 30-minute limit")
        return remaining

    def append(self, name):
        self.remaining()
        if name != self.current_family:
            raise AssertionError(f"unexpected contract family {name}, expected {self.current_family}")
        elapsed = self.clock() - self.started
        super().append(name)
        event = {"event": "family_correctness", "family": name, "wall_seconds": elapsed, "completed": len(self), "total": len(self.names)}
        with (self.output / "family-progress.jsonl").open("a") as stream:
            stream.write(json.dumps(event) + "\n")
        print(json.dumps(event), flush=True)
        self.started = self.clock()


def edge_setup_queries(edges, batch_rows=32):
    """Unmeasured graph construction; preserve edge order and multiplicity."""
    for start in range(0, len(edges), batch_rows):
        batch = edges[start:start + batch_rows]
        identifiers = sorted({identifier for edge in batch for identifier in edge})
        bindings = ",".join(f"(n{identifier}:User {{id:{identifier}}})" for identifier in identifiers)
        creations = ",".join(f"(n{source})-[:LINK {{weight:1}}]->(n{target})" for source, target in batch)
        yield "MATCH " + bindings + " CREATE " + creations


def point_probe_ids(count: int, profile: str, rng: random.Random) -> list[int]:
    """Registered full-size random hits followed by ten-percent missing IDs."""
    hits = rng.sample(range(count), min(10 if profile == "smoke" else 1000, count))
    return hits + [count + ordinal for ordinal in range(max(1, len(hits) // 10))]


def typed(value):
    if value is None:
        return ["null", None]
    if isinstance(value, bool):
        return ["boolean", value]
    if isinstance(value, int):
        return ["integer", value]
    if isinstance(value, float):
        return ["double", value]
    if isinstance(value, str):
        return ["string", value]
    if isinstance(value, list):
        return ["list", [typed(item) for item in value]]
    if isinstance(value, dict):
        return ["map", {key: typed(item) for key, item in sorted(value.items())}]
    raise ValueError(f"unsupported oracle type: {type(value).__name__}")


def cypher(value, escaped_keys=False):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return "'" + value.replace("\\", "\\\\").replace("'", "\\'") + "'"
    if isinstance(value, (int, float)):
        return str(value)
    if isinstance(value, dict):
        def name(key):
            if not escaped_keys and re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", key):
                return key
            return "`" + key.replace("`", "``") + "`"
        return "{" + ",".join(f"{name(key)}:{cypher(item, escaped_keys)}" for key, item in value.items()) + "}"
    raise ValueError("unsupported literal")


def compare_rows(actual, expected, ordered=False):
    wanted = [[typed(value) for value in row] for row in expected]
    if not ordered:
        key = lambda row: json.dumps(row, sort_keys=True, separators=(",", ":"))
        actual, wanted = sorted(actual, key=key), sorted(wanted, key=key)
    if actual != wanted:
        raise AssertionError(f"full-result mismatch: expected {wanted[:3]!r}, got {actual[:3]!r}; counts {len(wanted)}/{len(actual)}")


class NativeWorker:
    def __init__(self, binary: Path, database: Path, evidence: Path | None = None):
        self.process = subprocess.Popen([str(binary.resolve()), str(database)], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.reader = ThreadPoolExecutor(max_workers=1)
        self.trace = []
        self.evidence = evidence

    def retain(self, value):
        if isinstance(value, str) and len(value) > 4096 and self.evidence is not None:
            data = value.encode(); digest = hashlib.sha256(data).hexdigest()
            path = self.evidence / "inputs" / digest
            path.parent.mkdir(exist_ok=True)
            if not path.exists():
                path.write_bytes(data)
            return {"utf8_blob": str(path.relative_to(self.evidence)), "sha256": digest, "bytes": len(data)}
        if isinstance(value, list):
            return [self.retain(item) for item in value]
        if isinstance(value, dict):
            return {key: self.retain(item) for key, item in value.items()}
        return value

    def call(self, *fields, expected=None, ordered=False, error_contains=None):
        family_budget = getattr(self, "family_budget", None)
        if family_budget is not None:
            family_budget.remaining()
        if any("\n" in str(field) or "\t" in str(field) for field in fields):
            raise ValueError("worker fields cannot contain line delimiters")
        self.process.stdin.write("\t".join(map(str, fields)) + "\n")
        self.process.stdin.flush()
        try:
            operation_timeout = float(os.environ.get("ZYX_CONTRACT_OPERATION_TIMEOUT_SECONDS", "120"))
            if family_budget is not None:
                operation_timeout = min(operation_timeout, family_budget.remaining())
            line = self.reader.submit(self.process.stdout.readline).result(timeout=operation_timeout)
        except TimeoutError:
            self.process.kill()
            raise RuntimeError("native correctness operation exceeded its configured timeout")
        if not line:
            raise RuntimeError("worker ended: " + self.process.stderr.read()[:1000])
        response = json.loads(line)
        if error_contains is not None:
            observed = response.get("error", "").lower()
            quota_denial = error_contains.lower() == "byte" and observed == "hierarchical resource admission denied"
            if response.get("ok") or (error_contains.lower() not in observed and not quota_denial):
                raise AssertionError(f"expected {error_contains!r} failure, got {response!r}")
        elif not response.get("ok"):
            raise RuntimeError(response.get("error", "worker failure"))
        if expected is not None:
            compare_rows(response.get("rows", []), expected, ordered)
        self.trace.append({"command": self.retain(list(fields)), "expected": self.retain(expected), "ordered": ordered,
                           "expected_error_contains": error_contains,
                           "response_sha256": hashlib.sha256(line.encode()).hexdigest(),
                           "observed_rows": len(response.get("rows", [])),
                           "ok": response.get("ok"), "error": response.get("error")})
        return response

    def close(self):
        self.process.stdin.close()
        try:
            code = self.process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.process.kill(); self.process.wait(); code = -1
        self.reader.shutdown(wait=True)
        if code:
            raise RuntimeError(f"worker exit {code}: {self.process.stderr.read()[:1000]}")


def validate_high_cardinality_aggregate(worker, count, unsupported):
    query = "MATCH (n:Aggregate) RETURN n.id AS id,count(*) AS count"
    try:
        worker.call("QUERY", query, expected=[[i, 1] for i in range(count)])
        return
    except RuntimeError as error:
        # This exact full-output variant is a documented, observed QUERY-limit
        # case. Skewed/small groups and all unrelated failures remain mandatory.
        if count != 100000 or str(error) != "hierarchical resource admission denied":
            raise
        unsupported.append({"variant": "aggregate_100000_groups_owning_output",
                            "status": "unsupported_at_fixed_query_budget", "query": query,
                            "error": str(error), "latency_comparison": "N/A",
                            "validation": "all typed group/count pairs checked in bounded partitions; this does not make the rejected full-output query a success"})
    worker.call("QUERY", "MATCH (n:Aggregate) RETURN count(*) AS count", expected=[[count]])
    for start in range(0, count, 1000):
        worker.call("QUERY", f"MATCH (n:Aggregate) WHERE n.id >= {start} AND n.id < {start + 1000} RETURN n.id AS id,count(*) AS count",
                    expected=[[i, 1] for i in range(start, start + 1000)])


def run_native(binary: Path, output: Path, profile="smoke", seed=42, family=None):
    global _active_family_progress
    if family is not None and family not in NATIVE_FAMILY_ORDER:
        raise ValueError("unsupported isolated workload family")
    if profile not in {"smoke", "contract"}:
        raise ValueError("unknown preflight profile")
    if output.exists():
        raise ValueError("evidence output must be a fresh private directory")
    output.mkdir(parents=True)
    registration = {"profile": profile, "seed": seed, "performance_timing": False,
                    "worker_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                    "runner_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    "resources": {"threads": 1, "query_materialization_bytes": 67108864,
                                  "budget_probe_bytes": 4096 if profile == "smoke" else 4194304},
                    "lookup_index": "persistent native User.id property index, matching Kuzu User primary key; separate transient User.age schema-epoch probe",
                    "four_client_mode": "four native threads, separate query engines; explicit conflict interleavings also checked"}
    (output / "registration.json").write_text(json.dumps(registration, indent=2) + "\n")
    count = 32 if profile == "smoke" else 10000
    unsupported = []
    rng = random.Random(seed)
    users = [{"id": i, "country": f"country-{i % 8}", "score": i % 7, "age": 18 + i % 53} for i in range(count)]
    families = ContractFamilyProgress(output, (family,) if family else NATIVE_FAMILY_ORDER)
    _active_family_progress = families
    families.sampling_phase = "setup"
    with tempfile.TemporaryDirectory(prefix="zyx-contract-") as directory:
        worker = NativeWorker(binary, Path(directory) / "database.zyx", output)
        worker.family_budget = families
        try:
            registration["actual_format"] = worker.call("FORMAT")
            (output / "registration.json").write_text(json.dumps(registration, indent=2) + "\n")
            for start in range(0, count, 100):
                query = "CREATE " + ",".join(f"(:User {cypher(row)})" for row in users[start:start + 100])
                worker.call("QUERY", query)
            # Match Kuzu's User primary-key lookup condition before point,
            # traversal and mixed operations; keep this baseline index alive.
            worker.call("QUERY", "CREATE INDEX contract_base_user_id FOR (n:User) ON (n.id)")
            # Every expected row is computed from inputs, never copied from another engine.
            model = {row["id"]: row["score"] for row in users}
            plateau = []
            if family is None or family == 'point':
                for index in point_probe_ids(count, profile, rng):
                    worker.call("QUERY", f"MATCH (n:User {{id:{index}}}) RETURN n.id AS id,n.age AS age",
                                expected=[[index, users[index]["age"]]] if index < count else [])
                families.append("point")
            if family is None or family == 'scan':
                for fraction in (0.003, 0.37, 1.0):
                    boundary = max(1, int(count * fraction))
                    worker.call("QUERY", f"MATCH (n:User) WHERE n.id < {boundary} RETURN n.id AS id,n.country AS country",
                                expected=[[row["id"], row["country"]] for row in users if row["id"] < boundary])
                families.append("scan")
            if family is None or family == 'traversal':
                edges = [(0, 1), (0, 1), (0, 0), (1, 2), (2, 3)] + [(0, target) for target in range(1, count)]
                for query in edge_setup_queries(edges):
                    worker.call("QUERY", query)
                worker.call("QUERY", "MATCH (a:User {id:0})-[r:LINK]->(b) RETURN b.id AS id,r.weight AS weight",
                            expected=[[target, 1] for source, target in edges if source == 0])
                worker.call("QUERY", "MATCH (a:User {id:0})-[:LINK]->(b) RETURN b.id AS id,b.age AS age,b.country AS country,b.score AS score",
                            expected=[[target, users[target]["age"], users[target]["country"], users[target]["score"]] for source, target in edges if source == 0])
                worker.call("QUERY", "MATCH (a:User {id:0})-[:LINK]->(b)-[:LINK]->(c) RETURN b.id AS middle,c.id AS id",
                            expected=[[b, d] for a, b in edges for c, d in edges if a == 0 and b == c])
                families.append("traversal")
            if family is None or family == 'aggregate':
                groups = {}
                for row in users:
                    groups[row["country"]] = groups.get(row["country"], 0) + 1
                worker.call("QUERY", "MATCH (n:User) RETURN n.country AS country,count(*) AS total",
                            expected=[[key, value] for key, value in groups.items()])
                worker.call("QUERY", "MATCH (n:User) RETURN DISTINCT n.id AS id", expected=[[row["id"]] for row in users])
                aggregate_count = 32 if profile == "smoke" else 100000
                aggregate_rows = [{"id": i, "group": "dominant" if i < aggregate_count * 9 // 10 else f"g-{i}"} for i in range(aggregate_count)]
                for start in range(0, aggregate_count, 1000):
                    worker.call("QUERY", "CREATE " + ",".join(f"(:Aggregate {cypher(row)})" for row in aggregate_rows[start:start + 1000]))
                skew = {}
                for row in aggregate_rows:
                    skew[row["group"]] = skew.get(row["group"], 0) + 1
                worker.call("QUERY", "MATCH (n:Aggregate) RETURN n.group AS group,count(*) AS count", expected=[[key, value] for key, value in skew.items()])
                validate_high_cardinality_aggregate(worker, aggregate_count, unsupported)
                families.append("aggregate")
            if family is None or family == 'sort':
                sorted_users = sorted(users, key=lambda row: (-row["score"], row["id"]))
                for k in (1, 100, 10000):
                    worker.call("QUERY", f"MATCH (n:User) RETURN n.id AS id,n.score AS score ORDER BY score DESC,id LIMIT {k}",
                                expected=[[row["id"], row["score"]] for row in sorted_users[:k]], ordered=True)
                worker.call("QUERY", "MATCH (n:User) RETURN n.id AS id,n.score AS score ORDER BY score DESC,id",
                            expected=[[row["id"], row["score"]] for row in sorted_users], ordered=True)
                worker.call("QUERY", "CREATE (:SortNull {id:0,score:9}),(:SortNull {id:1,score:null}),(:SortNull {id:2})")
                worker.call("QUERY", "MATCH (n:SortNull) RETURN n.id AS id,n.score AS score ORDER BY score DESC,id",
                            expected=[[1, None], [2, None], [0, 9]], ordered=True)
                families.append("sort")
            if family is None or family == 'parameters':
                template = "MATCH (n:User) WHERE n.id = $id RETURN n.id AS id,n.country AS country"
                for index in [rng.randrange(count) for _ in range(100)]:
                    expected = [[index, users[index]["country"]]]
                    worker.call("PARAM", template, "id", "integer", index, expected=expected)
                    country = users[index]["country"]
                    worker.call("PARAM", "MATCH (n:User) WHERE n.country = $country RETURN n.id AS id", "country", "string", country,
                                expected=[[row["id"]] for row in users if row["country"] == country])
                    worker.call("PARAM", "MATCH (n:User) WHERE n.id < $limit RETURN n.id AS id", "limit", "integer", index,
                                expected=[[row["id"]] for row in users if row["id"] < index])
                    worker.call("CLEAR_CACHE")
                    worker.call("PARAM", template, "id", "integer", index, expected=expected)
                worker.call("QUERY", "CREATE INDEX contract_user_age FOR (n:User) ON (n.age)")
                worker.call("PARAM", template, "id", "integer", 0, expected=[[0, users[0]["country"]]])
                worker.call("QUERY", "MATCH (n:User) WHERE n.id < 10 RETURN n.id AS id", expected=[[i] for i in range(min(10, count))])
                worker.call("QUERY", "DROP INDEX contract_user_age")
                worker.call("PARAM", template, "id", "integer", 1, expected=[[1, users[1]["country"]]])
                families.append("parameters")
            if family is None or family == 'dynamic':
                worker.call("QUERY", "CREATE INDEX contract_dynamic_id FOR (n:Dynamic) ON (n.id)")
                dynamic_count = 12 if profile == "smoke" else 10000
                for index in range(dynamic_count):
                    width = (1, 8, 64)[index % 3]
                    properties = {"id": index, **{f"field_{key}": key + index for key in range(width)}, f"unique_{index}": index}
                    worker.call("QUERY", f"CREATE (:Dynamic {cypher(properties)})")
                    old_properties = dict(properties)
                    worker.call("BEGIN", "dynamic_old", "read")
                    worker.call("QUERY", f"MATCH (n:Dynamic {{id:{index}}}) SET n.field_0 = 'changed' REMOVE n.unique_{index} RETURN n.id AS id", expected=[[index]])
                    properties["field_0"] = "changed"; del properties[f"unique_{index}"]
                    if width > 1:
                        worker.call("QUERY", f"MATCH (n:Dynamic {{id:{index}}}) SET n.field_1 = null RETURN n.field_1 AS value", expected=[[None]])
                        del properties["field_1"]
                    worker.call("QUERY", f"MATCH (n:Dynamic {{id:{index}}}) RETURN properties(n) AS fields,n.not_registered AS missing", expected=[[properties, None]])
                    worker.call("TXQUERY", "dynamic_old", f"MATCH (n:Dynamic {{id:{index}}}) RETURN properties(n) AS fields", expected=[[old_properties]])
                    worker.call("ROLLBACK", "dynamic_old")
                families.append("dynamic")
            if family is None or family == 'wide_large':
                worker.call("QUERY", "CREATE INDEX contract_wide_id FOR (n:Wide) ON (n.id)")
                for width in (8, 128):
                    for payload_size in (32768, 1048576):
                        body = "x" * payload_size
                        for ordinal in range(1 if profile == "smoke" else 1000):
                            identifier = (width + payload_size) * 1000 + ordinal
                            properties = {"id": identifier, **{f"field_{key}": key for key in range(width)}}
                            worker.call("PARAM", f"CREATE (n:Wide {cypher(properties)}) SET n.body = $body RETURN n.id AS id", "body", "string", body, expected=[[identifier]])
                            properties["body"] = body
                            worker.call("QUERY", f"MATCH (n:Wide {{id:{identifier}}}) SET n.field_0 = 77 RETURN n.id AS id", expected=[[identifier]])
                            properties["field_0"] = 77
                            worker.call("QUERY", f"MATCH (n:Wide {{id:{identifier}}}) RETURN n.field_0 AS value", expected=[[77]])
                            worker.call("QUERY", f"MATCH (n:Wide {{id:{identifier}}}) RETURN properties(n) AS fields", expected=[[properties]])
                families.append("wide_large")
            if family is None or family == 'write':
                for batch in (1, 100, 1000):
                    worker.call("QUERY", "CREATE " + ",".join(f"(:Written {{id:{batch * 10000 + i},value:1}})" for i in range(batch)))
                    worker.call("QUERY", f"MATCH (n:Written) WHERE n.id >= {batch * 10000} AND n.id < {batch * 10000 + batch} SET n.value = 1 RETURN n.id AS id", expected=[[batch * 10000 + i] for i in range(batch)])
                    worker.call("QUERY", "MATCH (n:Written) SET n.value = 2 RETURN n.id AS id", expected=[[batch * 10000 + i] for i in range(batch)])
                    worker.call("QUERY", "MATCH (n:Written) REMOVE n.value RETURN n.id AS id,n.value AS missing", expected=[[batch * 10000 + i, None] for i in range(batch)])
                    worker.call("QUERY", "MATCH (n:Written) SET n.value = 1 RETURN n.id AS id", expected=[[batch * 10000 + i] for i in range(batch)])
                    worker.call("REOPEN")
                    worker.call("QUERY", f"MATCH (n:Written) WHERE n.id >= {batch * 10000} AND n.id < {batch * 10000 + batch} RETURN n.id AS id,n.value AS value", expected=[[batch * 10000 + i, 1] for i in range(batch)])
                    worker.call("QUERY", "MATCH (n:Written) DELETE n")
                worker.call("QUERY", "MATCH (n:Written) RETURN count(*) AS count", expected=[[0]])
                families.append("write")
            if family is None or family == 'mixed':
                model = {row["id"]: row["score"] for row in users}
                mixed_operations = 100 if profile == "smoke" else 100000
                for operation in range(mixed_operations):
                    index = rng.randrange(count)
                    if operation % 10 == 0:
                        model[index] += 1
                        worker.call("QUERY", f"MATCH (n:User {{id:{index}}}) SET n.score = {model[index]} RETURN n.score AS score", expected=[[model[index]]])
                    else:
                        worker.call("QUERY", f"MATCH (n:User {{id:{index}}}) RETURN n.score AS score", expected=[[model[index]]])
                # Four staged clients exercise conflicts explicitly; no claim of parallel scheduling.
                for client in range(4):
                    worker.call("BEGIN", f"client{client}", "write")
                    worker.call("TXQUERY", f"client{client}", "MATCH (n:User {id:0}) RETURN n.score AS score", expected=[[model[0]]])
                    worker.call("TXQUERY", f"client{client}", f"MATCH (n:User {{id:{client}}}) SET n.score = {model[client] + 1} RETURN n.id AS id", expected=[[client]])
                worker.call("COMMIT", "client0"); model[0] += 1
                for client in range(1, 4):
                    worker.call("COMMIT", f"client{client}", error_contains="fence")
                    worker.call("RELEASE", f"client{client}")
                threaded_operations = 20 if profile == "smoke" else 25000
                worker.call("MIXED_THREADS", threaded_operations)
                for client in range(4):
                    model[client] += (threaded_operations + 9) // 10
                    worker.call("QUERY", f"MATCH (n:User {{id:{client}}}) RETURN n.score AS score", expected=[[model[client]]])
                families.append("mixed")
            if family is None or family == 'snapshot':
                worker.call("BEGIN", "old", "read")
                worker.call("TXQUERY", "old", "MATCH (n:User {id:0}) RETURN n.score AS score", expected=[[model[0]]])
                for operation in range(8 if profile == "smoke" else 10000):
                    model[0] += 1
                    worker.call("QUERY", f"MATCH (n:User {{id:0}}) SET n.score = {model[0]} RETURN n.score AS score", expected=[[model[0]]])
                old_score = model[0] - (8 if profile == "smoke" else 10000)
                worker.call("QUERY", "MATCH (n:User {id:0}) DETACH DELETE n")
                worker.call("TXQUERY", "old", "MATCH (n:User {id:0}) RETURN n.score AS score", expected=[[old_score]])
                worker.call("ROLLBACK", "old"); worker.call("REOPEN"); worker.call("INTEGRITY")
                worker.call("QUERY", "MATCH (n:User {id:0}) RETURN n.score AS score", expected=[])
                families.append("snapshot")
            if family is None or family == 'maintenance':
                plateau = []
                warmup_windows = 3 if family == "maintenance" and profile == "contract" else 0
                for window in range(10 + warmup_windows):
                    families.sampling_phase = "warmup" if window < warmup_windows else "measure"
                    before_window = worker.call("STATS")
                    churn_count = 1 if profile == "smoke" else 10000
                    for start in range(0, churn_count, 100):
                        identifiers = [window * churn_count + ordinal for ordinal in range(start, min(start + 100, churn_count))]
                        worker.call("PARAM", "CREATE " + ",".join(f"(:Churn {{id:{identifier},body:$body}})" for identifier in identifiers), "body", "string", "y" * 8192)
                    worker.call("QUERY", "MATCH (n:Churn) DELETE n RETURN 1 AS deleted", expected=[[1]] * churn_count)
                    worker.call("MAINTAIN")
                    after_window = worker.call("STATS")
                    if window >= warmup_windows:
                        plateau.append({"file_bytes": after_window["file_bytes"], "rss_bytes": after_window["rss_bytes"],
                                        "read_bytes": after_window["read_bytes"] - before_window["read_bytes"],
                                        "write_bytes": after_window["write_bytes"] - before_window["write_bytes"]})
                    worker.call("REOPEN"); worker.call("INTEGRITY")
                    print(json.dumps({"event": "maintenance_window", "engine": "zyx-native", "window": window + 1, "phase": families.sampling_phase, "file_bytes": after_window["file_bytes"]}), flush=True)
                worker.call("QUERY", "MATCH (n:Churn) RETURN n.id AS id", expected=[])
                families.append("maintenance")
            if family is None or family == 'budget':
                if family == "budget":
                    for identifier in range(3):
                        worker.call("PARAM", f"CREATE (:Wide {{id:{identifier},body:$body}})", "body", "string", "b" * (2 * 1024 * 1024))
                    worker.call("QUERY", "MATCH (n:User {id:0}) DETACH DELETE n")
                worker.call("LIMIT", 4096 if profile == "smoke" else 4194304)
                worker.call("QUERY", "MATCH (n:Wide) RETURN properties(n) AS fields ORDER BY fields", error_contains="byte")
                worker.call("QUERY", "MATCH (n:Wide) RETURN n.id AS id,collect(n.body) AS bodies", error_contains="byte")
                worker.call("BEGIN", "budget_readonly", "read")
                worker.call("TXQUERY", "budget_readonly", "MATCH (n:Wide) RETURN properties(n) AS fields", error_contains="byte")
                worker.call("ROLLBACK", "budget_readonly")
                worker.call("READONLY")
                worker.call("QUERY", "MATCH (n:Wide) RETURN properties(n) AS fields", error_contains="byte")
                worker.call("READWRITE")
                worker.call("LIMIT", 67108864)
                worker.call("CANCEL", "MATCH (n:User) RETURN n.id AS id", error_contains="timeout")
                worker.call("REOPEN"); worker.call("INTEGRITY")
                worker.call("QUERY", "MATCH (n:User) RETURN count(*) AS count", expected=[[count - 1]])
                families.append("budget")
            if set(families) != ({family} if family else REQUIRED_FAMILIES):
                raise AssertionError("incomplete native family coverage")
            report = {"engine": "zyx-native", "profile": profile, "seed": seed, "families": families, "isolated_family": family,
                      "operations": len(worker.trace), "full_results_checked": True, "performance_timing": False,
                      "unsupported_variants": unsupported,
                      "support_scope": "all successful outputs fully checked; declared quota-rejected variants are N/A, never successes or latency ratios",
                      "maintenance_windows": plateau, "four_clients": "real native threads plus deterministic conflict interleavings; parallel timing deferred",
                      "binary_sha256": registration["worker_sha256"]}
            (output / "trace.json").write_text(json.dumps(worker.trace, indent=2) + "\n")
            report["trace_sha256"] = hashlib.sha256((output / "trace.json").read_bytes()).hexdigest()
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            return report
        finally:
            (output / "partial-trace.json").write_text(json.dumps(worker.trace, indent=2) + "\n")
            original_failure = sys.exc_info()[0] is not None
            try:
                worker.close()
            except RuntimeError as close_error:
                if not original_failure:
                    raise
                (output / "worker-close-after-failure.json").write_text(json.dumps({"error": str(close_error), "rule": "preserve the original case failure/timeout"}) + "\n")


def run_kuzu(output: Path, seed=42, profile="smoke", family=None):
    global _active_family_progress
    """Real-library smoke for equivalent fixed-schema workloads; no stand-ins or timing."""
    import kuzu
    if family is not None and family not in KUZU_FAMILY_ORDER:
        raise ValueError("unsupported isolated workload family")
    if profile not in {"smoke", "contract"}:
        raise ValueError("unknown preflight profile")
    if output.exists():
        raise ValueError("Kuzu evidence output must be fresh")
    output.mkdir(parents=True)
    rng = random.Random(seed)
    count = 32 if profile == "smoke" else 10000
    users = [{"id": i, "country": f"country-{i % 8}", "score": i % 7, "age": 18 + i % 53} for i in range(count)]
    trace = []
    retainer = object.__new__(NativeWorker); retainer.evidence = output
    families = ContractFamilyProgress(output, (family,) if family else KUZU_FAMILY_ORDER)
    _active_family_progress = families
    families.sampling_phase = "setup"
    with tempfile.TemporaryDirectory(prefix="kuzu-contract-") as directory:
        db = kuzu.Database(str(Path(directory) / "database"), buffer_pool_size=64 * 1024 * 1024,
                           max_num_threads=1)
        connection = kuzu.Connection(db, num_threads=1)
        def execute(query, expected=None, ordered=False, parameters=None):
            connection.set_query_timeout(max(1, int(families.remaining() * 1000)))
            cursor = connection.execute(query, parameters or {})
            if isinstance(cursor, list):
                raise RuntimeError("preflight requires one statement/result")
            observed = []
            try:
                while cursor.has_next():
                    row = cursor.get_next()
                    if expected is not None:
                        observed.append([typed(value) for value in row])
                if expected is not None:
                    compare_rows(observed, expected, ordered)
            finally:
                cursor.close()
            trace.append({"query": retainer.retain(query), "parameters": retainer.retain(parameters), "expected": retainer.retain(expected),
                          "ordered": ordered, "observed_rows": len(observed)})
        try:
            execute("CREATE NODE TABLE User(id INT64, country STRING, score INT64, age INT64, PRIMARY KEY(id))")
            execute("CREATE REL TABLE LINK(FROM User TO User, weight INT64)")
            for row in users:
                execute("CREATE (:User " + cypher(row, escaped_keys=True) + ")")
            model = {row["id"]: row["score"] for row in users}
            maintenance_windows = []
            if family is None or family == 'point':
                for index in point_probe_ids(count, profile, rng):
                    execute(f"MATCH (n:User {{id:{index}}}) RETURN n.id AS id,n.age AS age", [[index, users[index]["age"]]] if index < count else [])
                families.append("point")
            if family is None or family == 'scan':
                for fraction in (0.003, 0.37, 1.0):
                    boundary = max(1, int(count * fraction))
                    execute(f"MATCH (n:User) WHERE n.id < {boundary} RETURN n.id AS id,n.country AS country",
                            [[row["id"], row["country"]] for row in users if row["id"] < boundary])
                families.append("scan")
            if family is None or family == 'traversal':
                edges = [(0, 1), (0, 1), (0, 0), (1, 2), (2, 3)] + [(0, target) for target in range(1, count)]
                for query in edge_setup_queries(edges):
                    execute(query)
                execute("MATCH (a:User {id:0})-[r:LINK]->(b) RETURN b.id AS id,r.weight AS weight",
                        [[target, 1] for source, target in edges if source == 0])
                execute("MATCH (a:User {id:0})-[:LINK]->(b) RETURN b.id AS id,b.age AS age,b.country AS country,b.score AS score",
                        [[target, users[target]["age"], users[target]["country"], users[target]["score"]] for source, target in edges if source == 0])
                execute("MATCH (a:User {id:0})-[:LINK]->(b)-[:LINK]->(c) RETURN b.id AS middle,c.id AS id",
                        [[b, d] for a, b in edges for c, d in edges if a == 0 and b == c])
                families.append("traversal")
            if family is None or family == 'aggregate':
                groups = {}
                for row in users:
                    groups[row["country"]] = groups.get(row["country"], 0) + 1
                execute("MATCH (n:User) RETURN n.country AS country,count(*) AS total", [[key, value] for key, value in groups.items()])
                execute("MATCH (n:User) RETURN DISTINCT n.id AS id", [[row["id"]] for row in users])
                execute("CREATE NODE TABLE Aggregate(id INT64,`group` STRING,PRIMARY KEY(id))")
                aggregate_count = 32 if profile == "smoke" else 100000
                aggregate_rows = [{"id": i, "group": "dominant" if i < aggregate_count * 9 // 10 else f"g-{i}"} for i in range(aggregate_count)]
                for row in aggregate_rows:
                    execute("CREATE (:Aggregate " + cypher(row, escaped_keys=True) + ")")
                skew = {}
                for row in aggregate_rows:
                    skew[row["group"]] = skew.get(row["group"], 0) + 1
                execute("MATCH (n:Aggregate) RETURN n.`group` AS `group`,count(*) AS count", [[key, value] for key, value in skew.items()])
                execute("MATCH (n:Aggregate) RETURN n.id AS id,count(*) AS count", [[i, 1] for i in range(aggregate_count)])
                families.append("aggregate")
            if family is None or family == 'sort':
                ordered_users = sorted(users, key=lambda row: (-row["score"], row["id"]))
                for suffix, selected in [(" LIMIT 1", ordered_users[:1]), (" LIMIT 100", ordered_users[:100]), (" LIMIT 10000", ordered_users[:10000]), ("", ordered_users)]:
                    execute("MATCH (n:User) RETURN n.id AS id,n.score AS score ORDER BY score DESC,id" + suffix,
                            [[row["id"], row["score"]] for row in selected], True)
                execute("CREATE NODE TABLE SortNull(id INT64,score INT64,PRIMARY KEY(id))")
                execute("CREATE (:SortNull {id:0,score:9}),(:SortNull {id:1,score:null}),(:SortNull {id:2})")
                execute("MATCH (n:SortNull) RETURN n.id AS id,n.score AS score ORDER BY score DESC,id", [[1, None], [2, None], [0, 9]], True)
                families.append("sort")
            if family is None or family == 'parameters':
                for _ in range(100):
                    index = rng.randrange(count)
                    execute("MATCH (n:User) WHERE n.id = $id RETURN n.id AS id,n.country AS country",
                            [[index, users[index]["country"]]], parameters={"id": index})
                    country = users[index]["country"]
                    execute("MATCH (n:User) WHERE n.country = $country RETURN n.id AS id",
                            [[row["id"]] for row in users if row["country"] == country], parameters={"country": country})
                    execute("MATCH (n:User) WHERE n.id < $limit RETURN n.id AS id", [[i] for i in range(index)], parameters={"limit": index})
                families.append("parameters")
            # Fixed-schema wide projections check every field and the entire large body.
            if family is None or family == 'wide_large':
                for width in (8, 128):
                    table = f"Wide{width}"
                    execute(f"CREATE NODE TABLE {table}(id INT64,body STRING," + ",".join(f"field_{field} INT64" for field in range(width)) + ",PRIMARY KEY(id))")
                    for size in (32768, 1048576):
                        for ordinal in range(1 if profile == "smoke" else 1000):
                            identifier = (width + size) * 1000 + ordinal
                            properties = {"id": identifier, **{f"field_{field}": field for field in range(width)}}
                            execute(f"CREATE (n:{table} {cypher(properties, escaped_keys=True)}) SET n.body = $body", parameters={"body": "x" * size})
                            execute(f"MATCH (n:{table} {{id:{identifier}}}) SET n.field_0 = 77")
                            execute(f"MATCH (n:{table} {{id:{identifier}}}) RETURN n.id,n.body," + ",".join(f"n.field_{field}" for field in range(width)),
                                    [[identifier, "x" * size, 77, *range(1, width)]])
                families.append("wide_large")
            if family is None or family == 'write':
                execute("CREATE NODE TABLE Written(id INT64,value INT64,PRIMARY KEY(id))")
                for batch in (1, 100, 1000):
                    execute("CREATE " + ",".join(f"(:Written {{id:{i},value:1}})" for i in range(batch)))
                    execute("MATCH (n:Written) SET n.value = 1")
                    connection.close(); db.close()
                    db = kuzu.Database(str(Path(directory) / "database"), buffer_pool_size=64 * 1024 * 1024, max_num_threads=1)
                    connection = kuzu.Connection(db, num_threads=1)
                    execute("MATCH (n:Written) RETURN n.id,n.value", [[i, 1] for i in range(batch)])
                    execute("MATCH (n:Written) DELETE n")
                execute("MATCH (n:Written) RETURN count(*) AS count", [[0]])
                families.append("write")
            if family is None or family == 'mixed':
                model = {row["id"]: row["score"] for row in users}
                for operation in range(100 if profile == "smoke" else 100000):
                    index = rng.randrange(count)
                    if operation % 10 == 0:
                        model[index] += 1
                        execute(f"MATCH (n:User {{id:{index}}}) SET n.score = {model[index]} RETURN n.score", [[model[index]]])
                    else:
                        execute(f"MATCH (n:User {{id:{index}}}) RETURN n.score", [[model[index]]])
                from threading import Barrier
                barrier = Barrier(4)
                threaded_operations = 20 if profile == "smoke" else 25000
                def mixed_client(client):
                    local = kuzu.Connection(db, num_threads=1)
                    barrier.wait()
                    try:
                        for operation in range(threaded_operations):
                            families.remaining()
                            if operation % 10 == 0:
                                for attempt in range(1024):
                                    try:
                                        cursor = local.execute(f"MATCH (n:User {{id:{client}}}) SET n.score = n.score + 1")
                                        cursor.close(); break
                                    except RuntimeError as error:
                                        if not any(word in str(error).lower() for word in ("write transaction", "conflict")):
                                            raise
                                else:
                                    raise RuntimeError("Kuzu mixed retry limit exhausted")
                            else:
                                cursor = local.execute(f"MATCH (n:User {{id:{client}}}) RETURN n.score")
                                try:
                                    if not cursor.has_next():
                                        raise AssertionError("Kuzu mixed lost an owner")
                                    expected = model[client] + (operation + 9) // 10
                                    compare_rows([[typed(value) for value in cursor.get_next()]], [[expected]])
                                finally:
                                    cursor.close()
                    finally:
                        local.close()
                with ThreadPoolExecutor(max_workers=4) as clients:
                    list(clients.map(mixed_client, range(4)))
                for client in range(4):
                    model[client] += (threaded_operations + 9) // 10
                    execute(f"MATCH (n:User {{id:{client}}}) RETURN n.score", [[model[client]]])
                families.append("mixed")
            if family is None or family == 'snapshot':
                reader = kuzu.Connection(db, num_threads=1)
                try:
                    reader.execute("BEGIN TRANSACTION READ ONLY").close()
                    old_score = model[0]
                    def old_view():
                        cursor = reader.execute("MATCH (n:User {id:0}) RETURN n.score")
                        try:
                            compare_rows([[typed(value) for value in cursor.get_next()]], [[old_score]])
                        finally:
                            cursor.close()
                    old_view()
                    for _ in range(8 if profile == "smoke" else 10000):
                        model[0] += 1
                        execute(f"MATCH (n:User {{id:0}}) SET n.score = {model[0]}")
                    execute("MATCH (n:User {id:0}) DETACH DELETE n")
                    old_view(); reader.execute("ROLLBACK").close()
                finally:
                    reader.close()
                execute("MATCH (n:User {id:0}) RETURN n.score", [])
                families.append("snapshot")
            if family is None or family == 'maintenance':
                execute("CREATE NODE TABLE Churn(id INT64,body STRING,PRIMARY KEY(id))")
                maintenance_windows = []
                warmup_windows = 3 if family == "maintenance" and profile == "contract" else 0
                for window in range(10 + warmup_windows):
                    families.sampling_phase = "warmup" if window < warmup_windows else "measure"
                    churn_count = 1 if profile == "smoke" else 10000
                    for start in range(0, churn_count, 100):
                        identifiers = [window * churn_count + ordinal for ordinal in range(start, min(start + 100, churn_count))]
                        execute("CREATE " + ",".join(f"(:Churn {{id:{identifier},body:$body}})" for identifier in identifiers), parameters={"body": "y" * 8192})
                    execute("MATCH (n:Churn) DELETE n RETURN 1 AS deleted", [[1]] * churn_count)
                    connection.close(); db.close()
                    if window >= warmup_windows:
                        maintenance_windows.append({"file_bytes": sum(path.stat().st_size for path in Path(directory).rglob("*") if path.is_file())})
                    db = kuzu.Database(str(Path(directory) / "database"), buffer_pool_size=64 * 1024 * 1024, max_num_threads=1)
                    connection = kuzu.Connection(db, num_threads=1)
                    execute("MATCH (n:Churn) RETURN count(*)", [[0]])
                    print(json.dumps({"event": "maintenance_window", "engine": "kuzu", "window": window + 1, "phase": families.sampling_phase}), flush=True)
                families.append("maintenance")
            report = {"engine": "kuzu", "version": kuzu.__version__, "seed": seed,
                      "profile": profile, "operations": len(trace), "performance_timing": False,
                      "full_results_checked": True,
                      "families": families, "isolated_family": family,
                      "maintenance_windows": maintenance_windows,
                      "not_equivalent": {"dynamic": "fixed-schema tables cannot preserve arbitrary missing-key identity",
                                         "fence_interleaving": "native conservative fence errors differ; ordinary mixed workload is tested",
                                         "page_reclaim": "native journal/reclaimer/pin identity is engine-specific; old views and churn are tested",
                                         "budget": "native charged-byte/physical-readonly scratch contract is engine-specific"}}
            (output / "trace.json").write_text(json.dumps(trace, indent=2) + "\n")
            report["trace_sha256"] = hashlib.sha256((output / "trace.json").read_bytes()).hexdigest()
            (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            return report
        finally:
            connection.close(); db.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-worker", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--profile", choices=("smoke", "contract"), default="smoke")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--kuzu", action="store_true", help="also execute real Kuzu equivalent smoke workloads")
    args = parser.parse_args()
    native = run_native(args.native_worker, args.output, args.profile, args.seed)
    reports = [native]
    if args.kuzu:
        reports.append(run_kuzu(args.output / "kuzu", args.seed, args.profile))
    print(json.dumps(reports, indent=2))


if __name__ == "__main__":
    main()
