from __future__ import annotations

import argparse
import csv
import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class ScaleConfig:
    name: str
    users: int
    posts: int
    tags: int
    follows_per_user: int
    tags_per_post: int


@dataclass(frozen=True)
class GraphDataset:
    scale: str
    seed: int
    users: list[dict[str, str | int | float]]
    posts: list[dict[str, str | int | float]]
    tags: list[dict[str, str | int | float]]
    follows: list[dict[str, str | int]]
    authored: list[dict[str, str | int]]
    has_tag: list[dict[str, str | int]]


SCALES: dict[str, ScaleConfig] = {
    "smoke": ScaleConfig("smoke", users=100, posts=120, tags=30, follows_per_user=3, tags_per_post=2),
    "small": ScaleConfig("small", users=10_000, posts=12_000, tags=1_000, follows_per_user=5, tags_per_post=2),
    "medium": ScaleConfig("medium", users=100_000, posts=120_000, tags=5_000, follows_per_user=5, tags_per_post=2),
}

COUNTRIES = ["CN", "US", "DE", "JP", "SG", "BR", "IN", "FR"]


def generate_graph(config: ScaleConfig, seed: int = 42) -> GraphDataset:
    if config.users <= 0:
        raise ValueError("users must be greater than 0")
    if config.tags <= 0:
        raise ValueError("tags must be greater than 0")
    if config.posts < 0:
        raise ValueError("posts must be greater than or equal to 0")
    if config.follows_per_user < 0:
        raise ValueError("follows_per_user must be greater than or equal to 0")
    if config.tags_per_post < 0:
        raise ValueError("tags_per_post must be greater than or equal to 0")

    rng = random.Random(seed)
    users = [
        {
            "id": f"user-{i:06d}",
            "age": 18 + (i % 53),
            "country": COUNTRIES[i % len(COUNTRIES)],
            "score": round(rng.random() * 1000.0, 6),
        }
        for i in range(config.users)
    ]
    posts = [
        {
            "id": f"post-{i:06d}",
            "created_at": 1_700_000_000 + i,
            "score": round(rng.random() * 100.0, 6),
        }
        for i in range(config.posts)
    ]
    tags = [{"id": f"tag-{i:06d}", "rank": i} for i in range(config.tags)]

    follows: list[dict[str, str | int]] = []
    for i in range(config.users):
        for hop in range(1, config.follows_per_user + 1):
            follows.append(
                {
                    "src": f"user-{i:06d}",
                    "dst": f"user-{(i + hop) % config.users:06d}",
                    "weight": hop,
                }
            )

    authored = [
        {"src": f"user-{i % config.users:06d}", "dst": f"post-{i:06d}", "weight": 1}
        for i in range(config.posts)
    ]
    has_tag: list[dict[str, str | int]] = []
    for post_idx in range(config.posts):
        for offset in range(config.tags_per_post):
            tag_idx = (post_idx + offset) % config.tags
            has_tag.append(
                {
                    "src": f"post-{post_idx:06d}",
                    "dst": f"tag-{tag_idx:06d}",
                    "weight": offset + 1,
                }
            )

    return GraphDataset(config.name, seed, users, posts, tags, follows, authored, has_tag)


def _write_csv(path: Path, rows: Iterable[dict[str, object]], fieldnames: list[str]) -> int:
    count = 0
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)
            count += 1
    return count


def write_dataset(dataset: GraphDataset, output_dir: Path) -> dict[str, object]:
    output_dir.mkdir(parents=True, exist_ok=True)
    counts = {
        "users": _write_csv(output_dir / "users.csv", dataset.users, ["id", "age", "country", "score"]),
        "posts": _write_csv(output_dir / "posts.csv", dataset.posts, ["id", "created_at", "score"]),
        "tags": _write_csv(output_dir / "tags.csv", dataset.tags, ["id", "rank"]),
        "follows": _write_csv(output_dir / "follows.csv", dataset.follows, ["src", "dst", "weight"]),
        "authored": _write_csv(output_dir / "authored.csv", dataset.authored, ["src", "dst", "weight"]),
        "has_tag": _write_csv(output_dir / "has_tag.csv", dataset.has_tag, ["src", "dst", "weight"]),
    }
    manifest: dict[str, object] = {"scale": dataset.scale, "seed": dataset.seed, "counts": counts}
    (output_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate graph benchmark CSV dataset")
    parser.add_argument("--scale", choices=sorted(SCALES), default="smoke")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    dataset = generate_graph(SCALES[args.scale], seed=args.seed)
    manifest = write_dataset(dataset, args.output)
    print(json.dumps(manifest, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
