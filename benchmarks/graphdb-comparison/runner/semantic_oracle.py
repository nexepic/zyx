"""Independent full-result oracles for aggregate and Top-K observations.

Observers supply JSON-native rows from actual engine output. Row counts alone
are deliberately insufficient. Validation must run outside query timing.
"""
from __future__ import annotations

from collections import Counter
import math


def verify_groups(users: list[dict], rows: object) -> list[str]:
    expected = Counter(user["country"] for user in users)
    if not isinstance(rows, list):
        return ["missing aggregate rows"]
    observed = {}
    for row in rows:
        if not isinstance(row, list) or len(row) != 2:
            return ["aggregate rows must contain [country, count]"]
        country, count = row
        if type(country) is not str or type(count) is not int or count < 0:
            return ["aggregate key/count type mismatch"]
        if country in observed:
            return ["duplicate aggregate key"]
        observed[country] = count
    return [] if observed == dict(expected) else ["aggregate keys or counts differ"]


def verify_topk(users: list[dict], rows: object, limit: int = 100) -> list[str]:
    if type(limit) is not int or limit < 0:
        raise ValueError("limit must be a nonnegative integer")
    if not isinstance(rows, list):
        return ["missing Top-K rows"]
    scores = {}
    for user in users:
        identifier, score = user["id"], float(user["score"])
        if identifier in scores or not math.isfinite(score):
            raise ValueError("oracle dataset requires unique IDs and finite scores")
        scores[identifier] = score
    count = min(limit, len(users))
    if len(rows) != count:
        return ["Top-K row count differs"]
    observed = []
    for row in rows:
        if not isinstance(row, list) or len(row) != 1 or type(row[0]) is not str:
            return ["Top-K rows must contain one string ID"]
        observed.append(row[0])
    if len(set(observed)) != count:
        return ["duplicate Top-K ID"]
    if any(identifier not in scores for identifier in observed):
        return ["unknown Top-K ID"]
    actual_scores = [scores[identifier] for identifier in observed]
    expected_scores = sorted(scores.values(), reverse=True)[:count]
    # Equal scores may appear in any order, including ties at the limit boundary.
    # Unique IDs and the exact score sequence together prove a legal Top-K.
    return [] if actual_scores == expected_scores else ["Top-K order or selected scores differ"]


def verify_observations(users: list[dict], events: list[dict], databases: list[str],
                        workloads: list[str], iterations: int) -> dict:
    """Require one full observation for each measured iteration and workload."""
    if type(iterations) is not int or iterations <= 0:
        raise ValueError("iterations must be positive")
    verifiers = {"aggregation_count_by_group": verify_groups, "topk_property_sort": verify_topk}
    failures = []
    for database in databases:
        for workload in workloads:
            if workload not in verifiers:
                continue
            observations = [event for event in events if event.get("event") == "result"
                            and event.get("database") == database and event.get("workload") == workload]
            ordinals = [event.get("iteration") for event in observations]
            if (len(observations) != iterations or any(type(index) is not int for index in ordinals)
                    or set(ordinals) != set(range(iterations))):
                failures.append(f"{database}/{workload}: missing, invalid or duplicate observations")
                continue
            for event in observations:
                for error in verifiers[workload](users, event.get("rows")):
                    failures.append(f"{database}/{workload}/{event['iteration']}: {error}")
    return {"failures": failures,
            "scope": "complete country/count groups and ordered Top-K IDs; finite unique-ID input dataset"}
