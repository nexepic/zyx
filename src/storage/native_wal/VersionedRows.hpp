#pragma once
#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

#include "graph/storage/LogicalGraphRead.hpp"
namespace graph::storage::nativewal {
	std::pair<size_t, size_t> rowCost(const LogicalGraphNode &);
	std::pair<size_t, size_t> rowCost(const LogicalGraphEdge &);
	// All roots of one open Journal share this admission account, including
	// private transactions and superseded rows still reachable by an old reader.
	class VersionBudget {
		std::atomic<size_t> used{0};
		size_t maximum;

	public:
		explicit VersionBudget(size_t bytes) : maximum(bytes) {}
		void acquire(size_t n) {
			size_t prior = used.load();
			do {
				if (n > maximum - prior)
					throw std::length_error("Native graph version memory budget exceeded");
			} while (!used.compare_exchange_weak(prior, prior + n));
		}
		void release(size_t n) { used.fetch_sub(n); }
	};

	class VersionCharge {
		std::shared_ptr<VersionBudget> budget;
		size_t bytes;

	public:
		VersionCharge(std::shared_ptr<VersionBudget> b, size_t n) : budget(std::move(b)), bytes(n) {
			budget->acquire(n);
		}
		~VersionCharge() { budget->release(bytes); }
		VersionCharge(const VersionCharge &) = delete;
	};

	// A persistent AVL tree. Path copies share immutable entries (including
	// property payloads); a snapshot owns only its root until rows diverge.
	template<class Row>
	class VersionedRows {
		using Pair = std::pair<uint64_t, Row>;
		struct Entry {
			VersionCharge charge;
			size_t wire;
			Pair value;
			Entry(const std::shared_ptr<VersionBudget> &b, uint64_t key, Row row, size_t encoded, size_t heap) :
				charge(b, sizeof(Entry) + 64 + heap), wire(encoded), value(key, std::move(row)) {}
		};
		using Item = std::shared_ptr<const Entry>;
		struct Node;
		using Link = std::shared_ptr<const Node>;
		struct Node {
			VersionCharge charge;
			Item item;
			Link left, right;
			int height;
			Node(const std::shared_ptr<VersionBudget> &b, Item e, Link l, Link r) :
				charge(b, sizeof(Node) + 64), item(std::move(e)), left(std::move(l)), right(std::move(r)),
				height(1 + std::max(depth(left), depth(right))) {}
		};
		std::shared_ptr<VersionBudget> budget;
		Link root;
		size_t count = 0, wireSize = 0;
		static int depth(const Link &n) { return n ? n->height : 0; }
		Link node(Item e, Link l, Link r) const {
			return std::make_shared<Node>(budget, std::move(e), std::move(l), std::move(r));
		}
		Link balanced(const Item &e, const Link &l, const Link &r) const {
			if (depth(l) > depth(r) + 1) {
				if (depth(l->right) > depth(l->left)) {
					const auto &mid = l->right;
					return node(mid->item, node(l->item, l->left, mid->left), node(e, mid->right, r));
				}
				return node(l->item, l->left, node(e, l->right, r));
			}
			if (depth(r) > depth(l) + 1) {
				if (depth(r->left) > depth(r->right)) {
					const auto &mid = r->left;
					return node(mid->item, node(e, l, mid->left), node(r->item, mid->right, r->right));
				}
				return node(r->item, node(e, l, r->left), r->right);
			}
			return node(e, l, r);
		}
		Link replace(const Link &n, uint64_t key, const Item &e) const {
			if (!n)
				return node(e, {}, {});
			auto id = n->item->value.first;
			if (key < id)
				return balanced(n->item, replace(n->left, key, e), n->right);
			if (key > id)
				return balanced(n->item, n->left, replace(n->right, key, e));
			if (e)
				return node(e, n->left, n->right);
			if (!n->left)
				return n->right;
			if (!n->right)
				return n->left;
			auto next = n->right.get();
			while (next->left)
				next = next->left.get();
			return balanced(next->item, n->left, replace(n->right, next->item->value.first, {}));
		}
		const Entry *bound(uint64_t key) const {
			auto n = root.get();
			const Entry *best = nullptr;
			while (n) {
				const auto id = n->item->value.first;
				if (id == key)
					return n->item.get();
				if (id < key)
					n = n->right.get();
				else {
					best = n->item.get();
					n = n->left.get();
				}
			}
			return best;
		}

	public:
		using mapped_type = Row;
		explicit VersionedRows(std::shared_ptr<VersionBudget> b) : budget(std::move(b)) {}
		struct Iterator {
			const VersionedRows *rows;
			const Entry *entry;
			const Pair &operator*() const { return entry->value; }
			const Pair *operator->() const { return &entry->value; }
			Iterator &operator++() {
				*this = rows->upper_bound(entry->value.first);
				return *this;
			}
			bool operator==(const Iterator &) const = default;
		};
		Iterator begin() const { return {this, bound(0)}; }
		Iterator end() const { return {this, nullptr}; }
		Iterator upper_bound(uint64_t id) const { return {this, id == UINT64_MAX ? nullptr : bound(id + 1)}; }
		Iterator find(uint64_t id) const {
			auto e = bound(id);
			return {this, e && e->value.first == id ? e : nullptr};
		}
		size_t size() const { return count; }
		size_t encodedBytes() const { return wireSize; }
		bool contains(uint64_t id) const { return find(id) != end(); }
		const Row &at(uint64_t id) const {
			auto it = find(id);
			if (it == end())
				throw std::out_of_range("Unknown native graph ID");
			return it->second;
		}
		void insert_or_assign(uint64_t id, Row row) {
			auto [wire, heap] = rowCost(row);
			auto old = find(id);
			auto bytes = wireSize + wire - (old.entry ? old.entry->wire : 0);
			bool added = !old.entry;
			auto item = std::make_shared<Entry>(budget, id, std::move(row), wire, heap);
			root = replace(root, id, item);
			wireSize = bytes;
			count += added;
		}
		bool erase(uint64_t id) {
			auto old = find(id);
			if (!old.entry)
				return false;
			auto bytes = old.entry->wire;
			root = replace(root, id, {});
			wireSize -= bytes;
			--count;
			return true;
		}
	};
} // namespace graph::storage::nativewal
