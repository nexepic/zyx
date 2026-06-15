from __future__ import annotations

from pathlib import Path

from runner.adapters.base import BenchmarkAdapter, multihop_target_user_id, read_csv


class FakeAdapter(BenchmarkAdapter):
    def __init__(
        self,
        database: str,
        dataset_dir: Path,
        scale: str,
        profile: str = "scan",
        threads: int | None = None,
    ):
        super().__init__(database, dataset_dir, scale, profile, threads)
        self.users: list[dict[str, str]] = []
        self.posts: list[dict[str, str]] = []
        self.follows: list[dict[str, str]] = []
        self.write_counter = 0

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
        return self._reachable_within("user-000006", 6)

    def reachable_within_6(self) -> int:
        return self._reachable_within(multihop_target_user_id(6, self.scale), 6)

    def reachable_within_12(self) -> int:
        return self._reachable_within(multihop_target_user_id(12, self.scale), 12)

    def reachable_within_24(self) -> int:
        return self._reachable_within(multihop_target_user_id(24, self.scale), 24)

    def reachable_within_30(self) -> int:
        return self._reachable_within(multihop_target_user_id(30, self.scale), 30)

    def varlength_frontier_count(self) -> int:
        self._ensure_loaded()
        adjacency: dict[str, list[str]] = {}
        for edge in self.follows:
            adjacency.setdefault(edge["src"], []).append(edge["dst"])

        count = 0
        for user in self.users:
            if user["country"] != "CN":
                continue
            source = user["id"]
            first_hop = adjacency.get(source, [])
            count += len(first_hop)
            for middle in first_hop:
                count += len(adjacency.get(middle, []))
        return count

    def _reachable_within(self, target: str, max_depth: int) -> int:
        self._ensure_loaded()
        frontier = {"user-000001"}
        visited = set(frontier)
        adjacency: dict[str, set[str]] = {}
        for edge in self.follows:
            adjacency.setdefault(edge["src"], set()).add(edge["dst"])

        for _ in range(max_depth):
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

    def aggregation_count_by_group(self) -> int:
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

    def point_create_node(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        self.users.append(
            {
                "id": f"bench-user-{self.write_counter:06d}",
                "age": "41",
                "country": "ZZ",
                "score": str(float(self.write_counter)),
            }
        )
        return 1

    def point_create_edge(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        self.follows.append({"src": "user-000006", "dst": "user-000001", "weight": "1"})
        return 1

    def point_update_node_property(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        for user in self.users:
            if user["id"] == "user-000001":
                user["score"] = str(1000.0 + self.write_counter)
                return 1
        return 0

    def point_update_edge_property(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        target = "user-000004" if self.scale == "smoke" else "user-000006"
        for edge in self.follows:
            if edge["src"] == "user-000001" and edge["dst"] == target:
                edge["weight"] = str(10_000 + self.write_counter)
                return 1
        return 0

    def point_create_delete_edge(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        weight = str(-self.write_counter)
        self.follows.append({"src": "user-000006", "dst": "user-000001", "weight": weight})
        before = len(self.follows)
        self.follows = [
            edge
            for edge in self.follows
            if not (edge["src"] == "user-000006" and edge["dst"] == "user-000001" and edge["weight"] == weight)
        ]
        return 1 if len(self.follows) == before - 1 else 0

    def write_then_read_edge(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        weight = str(-1_000_000 - self.write_counter)
        self.follows.append({"src": "user-000007", "dst": "user-000001", "weight": weight})
        return sum(
            1
            for edge in self.follows
            if edge["src"] == "user-000007" and edge["dst"] == "user-000001" and edge["weight"] == weight
        )

    def post_persist_create_node(self) -> int:
        return self.point_create_node()

    def post_persist_create_edge(self) -> int:
        return self.point_create_edge()

    def write_then_one_hop_expand(self) -> int:
        self._ensure_loaded()
        self.write_counter += 1
        self.follows.append({"src": "user-000007", "dst": "user-000001", "weight": str(-2_000_000 - self.write_counter)})
        return sum(1 for edge in self.follows if edge["src"] == "user-000007")

    def index_seek_then_one_hop_expand(self) -> int:
        self._ensure_loaded()
        cn_users = {user["id"] for user in self.users if user["country"] == "CN"}
        return sum(1 for edge in self.follows if edge["src"] in cn_users)

    def index_seek_then_two_hop_expand(self) -> int:
        self._ensure_loaded()
        cn_users = {user["id"] for user in self.users if user["country"] == "CN"}
        outgoing_counts: dict[str, int] = {}
        for edge in self.follows:
            outgoing_counts[edge["src"]] = outgoing_counts.get(edge["src"], 0) + 1
        first_hop = [edge["dst"] for edge in self.follows if edge["src"] in cn_users]
        return sum(outgoing_counts.get(seed, 0) for seed in first_hop)

    def batch_create_edges_100(self) -> int:
        return self._batch_create_edges(100)

    def batch_create_edges_1000(self) -> int:
        return self._batch_create_edges(1000)

    def batch_create_edges_10000(self) -> int:
        return self._batch_create_edges(10000)

    def batch_create_edges_100_then_one_hop_expand(self) -> int:
        self._batch_create_edges(100, src="user-000007")
        return sum(1 for edge in self.follows if edge["src"] == "user-000007")

    def batch_create_edges_10000_then_one_hop_expand(self) -> int:
        self._batch_create_edges(10000, src="user-000008")
        return sum(1 for edge in self.follows if edge["src"] == "user-000008")

    def _batch_create_edges(self, count: int, src: str = "user-000006", dst: str = "user-000001") -> int:
        self._ensure_loaded()
        for _ in range(count):
            self.write_counter += 1
            self.follows.append({"src": src, "dst": dst, "weight": str(-3_000_000 - self.write_counter)})
        return count

    def _ensure_loaded(self) -> None:
        if not self.users and not self.follows:
            self.load_nodes_edges()
