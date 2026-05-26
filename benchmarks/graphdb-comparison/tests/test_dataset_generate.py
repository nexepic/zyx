from pathlib import Path

import pytest

from dataset.generate import ScaleConfig, generate_graph, write_dataset


def test_generate_graph_is_deterministic():
    config = ScaleConfig(name="unit", users=5, posts=4, tags=3, follows_per_user=2, tags_per_post=2)

    first = generate_graph(config, seed=7)
    second = generate_graph(config, seed=7)

    assert first == second
    assert len(first.users) == 5
    assert len(first.posts) == 4
    assert len(first.tags) == 3
    assert len(first.follows) == 10
    assert len(first.authored) == 4
    assert len(first.has_tag) == 8
    assert first.users[0]["id"] == "user-000000"
    assert first.posts[0]["id"] == "post-000000"
    assert first.tags[0]["id"] == "tag-000000"


def test_generate_graph_rejects_zero_users():
    config = ScaleConfig(name="unit", users=0, posts=1, tags=1, follows_per_user=0, tags_per_post=0)

    with pytest.raises(ValueError, match="users must be greater than 0"):
        generate_graph(config)


def test_generate_graph_rejects_zero_tags():
    config = ScaleConfig(name="unit", users=1, posts=1, tags=0, follows_per_user=0, tags_per_post=0)

    with pytest.raises(ValueError, match="tags must be greater than 0"):
        generate_graph(config)


@pytest.mark.parametrize(
    ("field", "message"),
    [
        ("posts", "posts must be greater than or equal to 0"),
        ("follows_per_user", "follows_per_user must be greater than or equal to 0"),
        ("tags_per_post", "tags_per_post must be greater than or equal to 0"),
    ],
)
def test_generate_graph_rejects_negative_counts(field: str, message: str):
    values = {
        "name": "unit",
        "users": 1,
        "posts": 1,
        "tags": 1,
        "follows_per_user": 0,
        "tags_per_post": 0,
    }
    values[field] = -1

    with pytest.raises(ValueError, match=message):
        generate_graph(ScaleConfig(**values))


def test_write_dataset_outputs_all_csv_files(tmp_path: Path):
    config = ScaleConfig(name="unit", users=3, posts=2, tags=2, follows_per_user=1, tags_per_post=1)
    graph = generate_graph(config, seed=11)

    manifest = write_dataset(graph, tmp_path)

    assert manifest["scale"] == "unit"
    for filename in [
        "users.csv",
        "posts.csv",
        "tags.csv",
        "follows.csv",
        "authored.csv",
        "has_tag.csv",
        "manifest.json",
    ]:
        assert (tmp_path / filename).exists(), filename
    assert (tmp_path / "users.csv").read_text().splitlines()[0] == "id,age,country,score"
    assert manifest["counts"] == {
        "users": 3,
        "posts": 2,
        "tags": 2,
        "follows": 3,
        "authored": 2,
        "has_tag": 2,
    }
