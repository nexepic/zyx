from __future__ import annotations

from pathlib import Path

from runner.adapters.base import BenchmarkAdapter, read_csv


class FakeAdapter(BenchmarkAdapter):
    def __init__(self, database: str, dataset_dir: Path, scale: str, profile: str = "scan"):
        super().__init__(database, dataset_dir, scale, profile)
        self.users: list[dict[str, str]] = []
        self.posts: list[dict[str, str]] = []
        self.follows: list[dict[str, str]] = []

    def setup(self) -> None:
        return None

    def teardown(self) -> None:
        self.users = []
        self.posts = []
        self.follows = []

    def load_nodes_edges(self) -> int:
        self.users = read_csv(self.dataset_dir / "users.csv")
        self.posts = read_csv(self.dataset_dir / "posts.csv")
        self.follows = read_csv(self.dataset_dir / "follows.csv")
        return len(self.users) + len(self.follows)

    def point_lookup_indexed(self) -> int:
        self._ensure_loaded()
        return 1 if any(user["id"] == "user-000001" for user in self.users) else 0

    def label_scan_filter(self) -> int:
        self._ensure_loaded()
        return sum(1 for user in self.users if user["country"] == "CN")

    def all_nodes_property_filter(self) -> int:
        self._ensure_loaded()
        return sum(1 for row in self.users if float(row["score"]) >= 900.0) + sum(
            1 for row in self.posts if float(row["score"]) >= 900.0
        )

    def label_multi_property_filter(self) -> int:
        self._ensure_loaded()
        return sum(1 for row in self.users if row["country"] == "CN" and int(row["age"]) >= 30)

    def relationship_type_scan(self) -> int:
        self._ensure_loaded()
        return len(self.follows)

    def relationship_property_filter(self) -> int:
        self._ensure_loaded()
        return sum(1 for row in self.follows if int(row["weight"]) == 1)

    def one_hop_expand(self) -> int:
        self._ensure_loaded()
        return sum(1 for edge in self.follows if edge["src"] == "user-000001")

    def two_hop_expand(self) -> int:
        self._ensure_loaded()
        first_hop = {edge["dst"] for edge in self.follows if edge["src"] == "user-000001"}
        return sum(1 for edge in self.follows if edge["src"] in first_hop)

    def shortest_path_chain(self) -> int:
        self._ensure_loaded()
        target = "user-000006"
        frontier = {"user-000001"}
        visited = set(frontier)
        adjacency: dict[str, set[str]] = {}
        for edge in self.follows:
            adjacency.setdefault(edge["src"], set()).add(edge["dst"])

        for _ in range(6):
            next_frontier = set()
            for node in frontier:
                for neighbor in adjacency.get(node, set()):
                    if neighbor == target:
                        return 1
                    if neighbor not in visited:
                        visited.add(neighbor)
                        next_frontier.add(neighbor)
            frontier = next_frontier
            if not frontier:
                break
        return 0

    def aggregation_group_by(self) -> int:
        self._ensure_loaded()
        return len({row["country"] for row in self.users})

    def topk_property_sort(self) -> int:
        self._ensure_loaded()
        return min(100, len(self.users))

    def property_equality_indexed(self) -> int:
        return self.label_scan_filter()

    def property_range_indexed(self) -> int:
        self._ensure_loaded()
        return sum(1 for row in self.users if 30 <= int(row["age"]) < 40)

    def _ensure_loaded(self) -> None:
        if not self.users and not self.follows:
            self.load_nodes_edges()
