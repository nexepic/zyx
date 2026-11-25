#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/utils/Sha256.hpp"
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
		size_t capacity() const { return maximum; }
		void release(size_t n) { used.fetch_sub(n); }
	};

	// Charge the actual vector allocations, including both buffers during a
	// reallocation. Once the old allocation is freed its admission is returned,
	// including when version-owned containers outlive the operation.
	class VersionMemoryResource final : public std::pmr::memory_resource {
		std::shared_ptr<VersionBudget> budget;
		void *do_allocate(size_t bytes, size_t alignment) override {
			budget->acquire(bytes);
			try {
				return std::pmr::new_delete_resource()->allocate(bytes, alignment);
			} catch (...) {
				budget->release(bytes);
				throw;
			}
		}
		void do_deallocate(void *p, size_t bytes, size_t alignment) noexcept override {
			std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
			budget->release(bytes);
		}
		bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

	public:
		explicit VersionMemoryResource(std::shared_ptr<VersionBudget> b) : budget(std::move(b)) {}
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
		void growTo(size_t n) {
			if (n > bytes) {
				budget->acquire(n - bytes);
				bytes = n;
			}
		}
	};

	class BaseInput {
	public:
		virtual ~BaseInput() = default;
		virtual void read(uint64_t id, size_t offset, std::span<std::byte> bytes) const = 0;
	};

	// A persistent AVL tree. Path copies share immutable entries (including
	// property payloads); a snapshot owns only its root until rows diverge.
	template<class Row>
	class VersionedRows {
		using Pair = std::pair<uint64_t, Row>;
		struct Record {
			size_t offset;
			std::array<std::byte, 32> digest;
			Row (*decode)(uint64_t, std::span<const std::byte>);
		};
		struct Entry {
			VersionCharge charge;
			size_t wire;
			Pair value;
			std::unique_ptr<Record> record;
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
			bool hasResident;
			Node(const std::shared_ptr<VersionBudget> &b, Item e, Link l, Link r) :
				charge(b, sizeof(Node) + 64), item(std::move(e)), left(std::move(l)), right(std::move(r)),
				height(1 + std::max(depth(left), depth(right))),
				hasResident(!item->record || (left && left->hasResident) || (right && right->hasResident)) {}
		};
		std::shared_ptr<VersionBudget> budget;
		Link root;
		size_t count = 0, wireSize = 0, diskRows = 0;
		std::shared_ptr<const BaseInput> input;
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
			mutable Item loaded{};
			size_t encodedBytes() const { return entry->wire; }
			uint64_t id() const { return entry->value.first; }
			bool onDisk() const { return bool(entry->record); }
			size_t offset() const { return entry->record->offset; }
			void readImage(std::span<std::byte> data) const {
				rows->input->read(id(), offset(), data);
				if (utils::Sha256::digest(data) != entry->record->digest)
					throw std::runtime_error("Immutable native base row changed");
			}
			const Pair &operator*() const {
				if (!onDisk())
					return entry->value;
				if (!loaded) {
					auto size = encodedBytes() - 16;
					VersionCharge scratch(rows->budget, 3 * size + 4096);
					std::vector<std::byte> data(size);
					readImage(data);
					auto row = entry->record->decode(id(), data);
					auto [wire, heap] = rowCost(row);
					loaded = std::make_shared<Entry>(rows->budget, entry->value.first, std::move(row), wire, heap);
				}
				return loaded->value;
			}
			const Pair *operator->() const { return &operator*(); }
			Iterator &operator++() {
				*this = rows->upper_bound(entry->value.first);
				return *this;
			}
			bool operator==(const Iterator &other) const { return rows == other.rows && entry == other.entry; }
		};
		// An ordered range pays for one lower-bound descent. A false callback
		// stops before materializing any subsequent row, including disk-backed rows.
		template<class Visitor>
		void visitAfter(uint64_t after, const Visitor &visit) const {
			auto walk = [&](auto &&self, const Node *n) -> bool {
				while (n) {
					if (n->item->value.first > after) {
						if (!self(self, n->left.get()) || !visit(Iterator{this, n->item.get()}))
							return false;
					}
					n = n->right.get();
				}
				return true;
			};
			walk(walk, root.get());
		}
		size_t residentCount() const { return count - diskRows; }
		// A disk record can only be introduced by loading an authenticated base.
		// Replacements and insertions are resident; deletions leave no live entry.
		template<class Visitor>
		bool visitResident(const Visitor &visit) const {
			auto walk = [&](auto &&self, const Node *n) -> bool {
				if (!n || !n->hasResident)
					return true;
				return self(self, n->left.get()) && (n->item->record || visit(n->item->value)) &&
					   self(self, n->right.get());
			};
			return walk(walk, root.get());
		}
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
		Iterator checked(uint64_t id) const {
			auto it = find(id);
			if (it == end())
				throw std::out_of_range("Unknown native graph ID");
			return it;
		}
		Row at(uint64_t id) const { return checked(id)->second; }
		// Decode the authenticated stream in key order. Construct one AVL node
		// per record directly instead of path-copying a private prefix for each ID.
		template<class Read>
		void loadSorted(size_t n, Read read) {
			auto build = [&](auto &&self, size_t length) -> Link {
				if (!length)
					return {};
				auto left = self(self, length / 2);
				auto item = read();
				wireSize += item->wire;
				diskRows += bool(item->record);
				auto right = self(self, length - length / 2 - 1);
				return node(std::move(item), std::move(left), std::move(right));
			};
			root = build(build, n);
			count = n;
		}
		Item residentRecord(uint64_t id, Row row) const {
			auto [wire, heap] = rowCost(row);
			return std::make_shared<Entry>(budget, id, std::move(row), wire, heap);
		}
		Item baseRecord(uint64_t id, size_t wire, size_t offset, const std::array<std::byte, 32> &digest,
						Row (*decode)(uint64_t, std::span<const std::byte>)) const {
			auto item = std::make_shared<Entry>(budget, id, Row{}, wire, 192);
			item->record = std::make_unique<Record>(Record{offset, digest, decode});
			return item;
		}
		void bindInput(std::shared_ptr<const BaseInput> file) { input = diskRows ? std::move(file) : nullptr; }
		void inheritInput(const VersionedRows &other) { bindInput(other.input); }
		void insert_or_assign(uint64_t id, Row row) {
			auto [wire, heap] = rowCost(row);
			auto old = find(id);
			auto bytes = wireSize + wire - (old.entry ? old.entry->wire : 0);
			bool added = !old.entry;
			auto item = std::make_shared<Entry>(budget, id, std::move(row), wire, heap);
			bool wasOnDisk = old.entry && old.onDisk();
			root = replace(root, id, item);
			if (wasOnDisk && !--diskRows)
				input.reset();
			wireSize = bytes;
			count += added;
		}
		bool erase(uint64_t id) {
			auto old = find(id);
			if (!old.entry)
				return false;
			auto bytes = old.entry->wire;
			bool wasOnDisk = old.onDisk();
			root = replace(root, id, {});
			if (wasOnDisk && !--diskRows)
				input.reset();
			wireSize -= bytes;
			--count;
			return true;
		}
	};
} // namespace graph::storage::nativewal
