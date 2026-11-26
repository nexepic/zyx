#include "Journal.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include <filesystem>
#include <system_error>
#include <utility>
#if defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
namespace graph::storage::nativewal {
	namespace {
		[[noreturn]] void fail(const char *action) { throw std::system_error(errno, std::generic_category(), action); }
		void readExact(int fd, uint64_t offset, std::span<std::byte> target);
		struct File : BaseInput {
			void read(uint64_t, size_t offset, std::span<std::byte> bytes) const override {
				readExact(fd, offset, bytes);
			}
			int fd;
			explicit File(int f) : fd(f) {
				if (fd < 0)
					fail("open native graph file");
			}
			~File() {
				if (fd >= 0)
					::close(fd);
			}
			File(File &&other) noexcept : fd(std::exchange(other.fd, -1)) {}
			File(const File &) = delete;
		};
		// Immutable entries keep their original authenticated offsets. Between
		// two length changes the same translation applies to every surviving ID.
		// Each published view maps directly to its own file, never to a prior map.
		struct LocatorRun {
			uint64_t firstId = 0;
			size_t original = 0, published = 0;
			bool startsRun(size_t from, size_t to) const { return from - original != to - published; }
		};
		class LocatedInput final : public BaseInput {
			VersionCharge charge;
			std::shared_ptr<const File> file;
			std::vector<LocatorRun> runs;

		public:
			LocatedInput(const std::shared_ptr<VersionBudget> &budget, std::shared_ptr<const File> f, size_t n) :
				charge(budget, sizeof(LocatedInput) + 64 + n * sizeof(LocatorRun)), file(std::move(f)) {
				runs.reserve(n);
			}
			void add(LocatorRun run) { runs.push_back(run); }
			void read(uint64_t id, size_t offset, std::span<std::byte> data) const override {
				auto next = std::upper_bound(runs.begin(), runs.end(), id,
											 [](uint64_t key, const LocatorRun &run) { return key < run.firstId; });
				if (next != runs.begin()) {
					const auto &run = *std::prev(next);
					require(offset >= run.original && offset - run.original <= SIZE_MAX - run.published,
							"Invalid native checkpoint locator");
					offset = run.published + (offset - run.original);
				}
				file->read(id, offset, data);
			}
		};
		struct CheckpointLocations {
			size_t count = 0;
			LocatorRun last;
			void observe(uint64_t id, size_t from, size_t to) {
				if (last.startsRun(from, to)) {
					last = {id, from, to};
					++count;
				}
			}
			template<class Table>
			void bind(Table &table, const std::shared_ptr<File> &file, size_t start,
					  const std::shared_ptr<VersionBudget> &budget) const {
				if (!count) {
					table.bindInput(file);
					return;
				}
				std::shared_ptr<LocatedInput> mapped;
				try {
					// Retention is optional. The independent image is already
					// durable and the old view remains authenticated and usable.
					mapped = std::make_shared<LocatedInput>(budget, file, count);
				} catch (const std::length_error &) {
					return;
				} catch (const std::bad_alloc &) {
					return;
				}
				if (count == 1) {
					mapped->add(last);
				} else {
					// Visit immutable metadata only, using the exact sizes verified
					// by the encoder. This pass neither reads nor constructs a row.
					CheckpointLocations layout;
					table.visitAfter(0, [&](auto it) {
						if (it.onDisk()) {
							auto previous = layout.count;
							layout.observe(it.id(), it.offset(), start + 16);
							if (layout.count != previous)
								mapped->add(layout.last);
						}
						start += it.encodedBytes();
						return true;
					});
					require(layout.count == count, "Native checkpoint locator count changed");
				}
				table.bindInput(std::move(mapped));
			}
		};
		void syncFile(int fd) {
			while (::fsync(fd) < 0) {
				if (errno != EINTR)
					fail("sync native graph file or directory");
			}
		}
		Identity identity() {
			Identity id;
			size_t used = 0;
			while (used < id.size()) {
				auto n = getrandom(id.data() + used, id.size() - used, 0);
				if (n < 0 && errno == EINTR)
					continue;
				if (n <= 0)
					fail("native graph identity");
				used += n;
			}
			return id;
		}
		std::string stageName(Span key) {
			const char *digits = "0123456789abcdef";
			std::string s = ".zyx-native-";
			for (auto b: key) {
				unsigned v = std::to_integer<unsigned>(b);
				s += digits[v >> 4];
				s += digits[v & 15];
			}
			return s;
		}
		void readExact(int fd, uint64_t offset, std::span<std::byte> target) {
			while (!target.empty()) {
				auto n = pread(fd, target.data(), target.size(), static_cast<off_t>(offset));
				if (n < 0 && errno == EINTR)
					continue;
				if (n < 0)
					fail("read native graph window");
				require(n > 0, "Short native graph window");
				offset += n;
				target = target.subspan(n);
			}
		}
		// The cursor authenticates bytes in consumption order. Refill never crosses
		// the selected receipt prefix, even if an unacknowledged suffix exists.
		class Cursor {
			int fd;
			size_t extent, filled = 0, next = 0, refillSize;
			VersionMemoryResource memory;
			std::pmr::vector<std::byte> window;
			bool authenticatedWindow = false, streamingReplay = false;

		public:
			utils::Sha256 hash;
			size_t position = 0;
			Cursor(int f, size_t end, std::shared_ptr<VersionBudget> budget, size_t limit, size_t start = 0) :
				fd(f), extent(end), refillSize(std::min({size_t(32768), end, limit / 16})), memory(std::move(budget)),
				window(refillSize, &memory), position(start) {
				require(!window.empty(), "Native recovery window budget exceeded");
			}
			size_t remaining() const { return extent - position; }
			void suspend() {
				// A single authenticated window is itself the replay proof. Keep
				// it until consumed. Other read-ahead can be refilled at position.
				if (authenticatedWindow && remaining())
					return;
				std::pmr::vector<std::byte>(&memory).swap(window);
				next = filled = 0;
			}
			void consume(std::span<std::byte> out) {
				require(out.size() <= remaining(), "Truncated native graph field");
				if (!out.empty() && window.empty())
					window.resize(refillSize);
				while (!out.empty()) {
					if (streamingReplay && next == filled && out.size() >= window.size()) {
						// A decoded field already owns its admitted destination.
						auto part = out.first(std::min(size_t(32768), out.size()));
						readExact(fd, position, part);
						hash.update(part);
						position += part.size();
						out = out.subspan(part.size());
						continue;
					}
					if (next == filled) {
						filled = std::min(window.size(), remaining());
						readExact(fd, position, std::span(window).first(filled));
						next = 0;
					}
					auto n = std::min(out.size(), filled - next);
					auto part = Span(window).subspan(next, n);
					if (!authenticatedWindow)
						hash.update(part);
					std::ranges::copy(part, out.begin());
					next += n;
					position += n;
					out = out.subspan(n);
				}
			}
			void authenticateWal(Span expected) {
				while (position < extent) {
					filled = std::min(window.size(), remaining());
					readExact(fd, position, std::span(window).first(filled));
					hash.update(Span(window).first(filled));
					position += filled;
				}
				require(std::ranges::equal(hash.finalize(), expected), "Native graph WAL receipt mismatch");
				position = next = 0;
				authenticatedWindow = filled == extent;
				if (!authenticatedWindow) {
					streamingReplay = true;
					// Bulk authentication is complete. Retain only lookahead for
					// small fields while larger fields read into their own buffers.
					refillSize = std::min(refillSize, size_t(4096));
					filled = 0;
					hash = utils::Sha256{};
				}
			}
			template<size_t N>
			auto fixed(bool readAhead = true) {
				std::array<std::byte, N> b;
				if (!readAhead && streamingReplay && next == filled) {
					require(N <= remaining(), "Truncated native graph field");
					readExact(fd, position, b);
					hash.update(b);
					position += N;
				} else {
					consume(b);
				}
				return b;
			}
			uint64_t num() {
				auto b = fixed<8>();
				return Decoder{b}.num();
			}
			size_t count(size_t max) {
				auto n = num();
				require(n <= max, "Native graph field exceeds limit");
				return size_t(n);
			}
		};
		Digest digest(Span b) { return utils::Sha256::digest(b); }
		// Separate regions for format, two confirmation receipts and two root slots.
		// Readers only authenticate the selected slot. Damage is an error.
		constexpr size_t rootPage = 4096, baseStart = 5 * rootPage;
		constexpr size_t receipt1 = rootPage, receipt2 = 4 * rootPage;
		struct EmbeddedRoot {
			Identity id{};
			size_t baseEnd = 0, end = 0;
			uint64_t generation = 0;
			Digest base{}, chain{};
			size_t slot = 0;
			Bytes encode() const {
				Encoder e(144);
				e.magic("ZYXrs001");
				e.raw(id);
				e.num(baseEnd);
				e.num(end);
				e.num(generation);
				e.raw(base);
				e.raw(chain);
				e.raw(digest(e.bytes));
				return std::move(e.bytes);
			}
		};
		void putAt(int fd, size_t pos, Span data) {
			while (!data.empty()) {
				auto n = pwrite(fd, data.data(), data.size(), pos);
				if (n < 0 && errno == EINTR)
					continue;
				if (n < 0)
					fail("write native embedded generation");
				require(n > 0, "Zero embedded write");
				pos += size_t(n);
				data = data.subspan(size_t(n));
			}
		}
		Bytes receipt(const EmbeddedRoot &root, Span record) {
			Encoder e(88);
			e.magic("ZYXok001");
			e.num(root.generation);
			e.num(root.slot);
			e.raw(digest(record));
			e.raw(digest(e.bytes));
			return std::move(e.bytes);
		}
		struct Confirmation {
			uint64_t sequence, slot;
			Digest rootHash;
		};
		std::optional<Confirmation> readReceipt(int fd, size_t offset) {
			std::array<std::byte, 88> wire{};
			readExact(fd, offset, wire);
			if (!std::ranges::equal(digest(Span(wire).first(56)), Span(wire).last(32)))
				return {};
			Decoder r{wire};
			r.magic("ZYXok001");
			Confirmation item{r.num(), r.count(1), {}};
			std::ranges::copy(r.take(32), item.rootHash.begin());
			return item;
		}
		EmbeddedRoot decodeRoot(Span bytes, size_t extent) {
			EmbeddedRoot root;
			require(bytes.size() == 144 && std::ranges::equal(digest(bytes.first(112)), bytes.last(32)),
					"Invalid confirmed native root");
			Decoder r{bytes};
			r.magic("ZYXrs001");
			std::ranges::copy(r.take(16), root.id.begin());
			root.baseEnd = r.count(extent);
			root.end = r.count(extent);
			root.generation = r.num();
			require(root.baseEnd >= baseStart + 144 && root.end >= root.baseEnd, "Invalid native root bounds");
			std::ranges::copy(r.take(32), root.base.begin());
			std::ranges::copy(r.take(32), root.chain.begin());
			return root;
		}
		EmbeddedRoot readRoot(int fd, size_t extent, bool &repair) {
			auto first = readReceipt(fd, receipt1), second = readReceipt(fd, receipt2);
			require(first || second, "No valid native confirmation receipt");
			if (first && second && first->sequence == second->sequence)
				require(first->rootHash == second->rootHash && first->slot == second->slot,
						"Conflicting native confirmation receipts");
			repair = !first || !second || first->sequence != second->sequence;
			auto selected = !second || (first && first->sequence >= second->sequence) ? *first : *second;
			std::array<std::byte, 144> bytes{};
			readExact(fd, (2 + selected.slot) * rootPage, bytes);
			require(digest(bytes) == selected.rootHash, "Invalid confirmed native root");
			auto root = decodeRoot(bytes, extent);
			root.slot = selected.slot;
			require(root.generation == selected.sequence, "Native confirmation revision mismatch");
			return root;
		}

		std::string_view signature(const State &s) {
			if (s.catalog || (s.unique && (s.unique->ownsProjection || s.unique->name.size() > 256)))
				return "ZYXng006";
			if (s.unique)
				return "ZYXng005";
			return s.declaration ? "ZYXng004" : "ZYXng003";
		}
		Bytes manifest(const State &s, uint64_t baseSequence, Span base, size_t length, const Digest &hash) {
			Encoder w(maxState + 256);
			w.magic(signature(s));
			w.raw(s.identity);
			w.num(baseSequence);
			w.num(s.sequence);
			w.num(length);
			w.raw(hash);
			w.raw(base);
			w.raw(digest(w.bytes));
			return std::move(w.bytes);
		}
	} // namespace
	struct Journal::Files {
		File directory;
		std::shared_ptr<File> authority;
		std::string main, log, stage;
		size_t alignment = 0;
		explicit Files(const std::filesystem::path &path) :
			directory(::open((path.has_parent_path() ? path.parent_path() : ".").c_str(),
							 O_DIRECTORY | O_RDONLY | O_CLOEXEC)),
			main(path.filename().string()), log(main + ".wal"),
			stage(stageName(utils::Sha256::digest(std::as_bytes(std::span(main))))) {
			// Keep legacy directory-exclusive writers out while allowing
			// independently locked databases to share this directory.
			if (flock(directory.fd, LOCK_SH | LOCK_NB) < 0)
				fail("lock native graph directory");
			// WAL, anchor and scratch names belong to their database throughout
			// its lifetime, including intervals when they do not exist on disk.
			require(!main.starts_with(".zyx-native-") && !main.ends_with(".base") && !main.ends_with(".wal"),
					"Managed native dependency is not a database entry point");
			struct stat info{};
			if (fstat(directory.fd, &info) < 0)
				fail("stat native graph directory");
			auto page = sysconf(_SC_PAGESIZE);
			require(page > 0 && info.st_blksize > 0, "Unknown filesystem or VM block size");
			alignment = static_cast<size_t>(std::max({long(4096), page, static_cast<long>(info.st_blksize)}));
			require(alignment <= 65536 && (alignment & (alignment - 1)) == 0, "Unsupported native WAL block alignment");
		}
		// The live Journal holds its gate and original file lock throughout the
		// audit. Duplicate the directory descriptor so relative names cannot
		// resolve against a different directory after a rename.
		explicit Files(const Files &held) :
			directory(fcntl(held.directory.fd, F_DUPFD_CLOEXEC, 0)), main(held.main), log(held.log),
			stage(held.stage), alignment(held.alignment) {}
		std::string anchor() const { return main + ".base"; }
		bool exists(const std::string &name) const {
			struct stat st{};
			if (fstatat(directory.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
				return true;
			if (errno == ENOENT)
				return false;
			fail("stat native dependency");
		}
		std::pair<std::shared_ptr<File>, size_t> openInput(const std::string &name, int access = O_RDONLY) {
			auto fd = openat(directory.fd, name.c_str(), access | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0 && errno == ENOENT && name == log)
				throw std::runtime_error("Native graph receipt requires missing WAL");
			if (fd < 0 && errno == ENOENT && name == anchor())
				throw std::runtime_error("Native graph receipt requires missing base");
			File owned(fd);
			auto f = std::make_shared<File>(std::move(owned));
			struct stat st{}, peer{};
			if (fstat(fd, &st) < 0)
				fail("stat native graph input");
			bool ownLink = name == main && st.st_nlink == 2 &&
						   fstatat(directory.fd, anchor().c_str(), &peer, AT_SYMLINK_NOFOLLOW) == 0 &&
						   peer.st_dev == st.st_dev && peer.st_ino == st.st_ino;
			require(st.st_nlink == 1 || ownLink, "Native graph main and WAL require private, unaliased files");
			require(S_ISREG(st.st_mode) && st.st_size >= 0 && uint64_t(st.st_size) <= SIZE_MAX,
					"Invalid native graph file extent");
			return {f, static_cast<size_t>(st.st_size)};
		}
		static void lockFile(int fd, bool ro) {
			if (flock(fd, (ro ? LOCK_SH : LOCK_EX) | LOCK_NB) < 0)
				fail("lock native graph main");
		}
		void checkAuthority() const {
			struct stat named{};
			if (fstatat(directory.fd, main.c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
				fail("stat native graph main authority");
			auto held = metadata(authority->fd);
			require(held.st_dev == named.st_dev && held.st_ino == named.st_ino, "Native graph base authority changed");
		}
		std::pair<std::shared_ptr<File>, size_t> lockMain(bool ro) {
			// Replacement also needs a writable directory. Require file write access
			// on the actual inode that grants mutable authority.
			auto opened = openInput(main, ro ? O_RDONLY : O_RDWR);
			lockFile(opened.first->fd, ro);
			authority = opened.first;
			// An opener may have paused after openat across a rename and unlock.
			// Never admit that superseded inode, even if its flock succeeds.
			checkAuthority();
			return opened;
		}
		std::optional<EmbeddedRoot> root;
		std::shared_ptr<File> stable;
		struct stat authenticated{};
		static struct stat metadata(int fd) {
			struct stat st{};
			if (fstat(fd, &st) < 0)
				fail("stat native base");
			return st;
		}
		static bool sameContentStamp(const struct stat &a, const struct stat &b) {
			return a.st_ino == b.st_ino && a.st_dev == b.st_dev && a.st_size == b.st_size &&
				   a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
		}
		static bool sameStamp(const struct stat &a, const struct stat &b) {
			return sameContentStamp(a, b) && a.st_ctim.tv_sec == b.st_ctim.tv_sec &&
				   a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
		}
		void remember(const std::shared_ptr<File> &f, const struct stat &before) {
			authenticated = metadata(f->fd);
			require(sameStamp(authenticated, before), "Native base changed during authentication");
			stable = f;
		}
		void retainBase() {
			require(stable && sameStamp(metadata(stable->fd), authenticated), "Authenticated native base changed");
			if (linkat(directory.fd, main.c_str(), directory.fd, anchor().c_str(), 0) < 0)
				fail("retain immutable native base");
			struct stat linked{};
			if (fstatat(directory.fd, anchor().c_str(), &linked, AT_SYMLINK_NOFOLLOW) < 0)
				fail("stat retained native base");
			require(linked.st_nlink == 2 && sameContentStamp(authenticated, linked),
					"Retained native base identity changed");
			syncFile(directory.fd);
		}
		// Main replacement removes the second link to this inode. Record that
		// exact transition before the first ACK; later appends must still reach
		// the same sealed base through the official database directory.
		void checkBaseName(bool first) {
			struct stat named{};
			if (fstatat(directory.fd, anchor().c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
				fail("stat required native base");
			require(stable && named.st_nlink == 1 &&
							(first ? sameContentStamp(authenticated, named) : sameStamp(authenticated, named)),
					"Required native base identity changed");
			if (first)
				authenticated = named;
		}
		static void writeComplete(int fd, Span bytes) {
			while (!bytes.empty()) {
				auto n = ::write(fd, bytes.data(), std::min(bytes.size(), size_t{32768}));
				if (n < 0 && errno == EINTR)
					continue;
				if (n < 0)
					fail("write native graph file");
				require(n > 0, "Zero native graph write");
				bytes = bytes.subspan(n);
			}
		}
		// Padding has no transaction-specific content. Tiny budgets can emit
		// these same authenticated bytes without retaining an aligned heap page.
		static constexpr std::array<std::byte, 4096> zeroPage{};
		template<class Emit>
		static void emitPadding(size_t size, Emit emit) {
			while (size) {
				auto chunk = Span(zeroPage).first(std::min(size, zeroPage.size()));
				emit(chunk);
				size -= chunk.size();
			}
		}
		std::unique_ptr<File> activeLog;
		template<class Emit>
		void appendStream(size_t offset, Emit emit) {
			require(bool(activeLog), "Missing native append authority");
			checkBaseName(false);
			struct stat st{}, named{};
			if (fstat(activeLog->fd, &st) < 0 || fstatat(directory.fd, log.c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
				fail("stat native append target");
			require(st.st_nlink == 1 && named.st_dev == st.st_dev && named.st_ino == st.st_ino && st.st_size >= 0 &&
							static_cast<uint64_t>(st.st_size) == offset,
					"Native WAL append extent or identity changed");
			emit(activeLog->fd);
			syncFile(activeLog->fd);
		}
		template<class Emit>
		void installStream(const std::string &name, Emit emit, const std::function<void()> &beforePublish = {}) {
			// Under the exclusive main lock, exactly one stage name is
			// reserved for this entry (including independent copies). A killed writer
			// can leave this inode behind, but repeated retries cannot accumulate stages.
			if (authority)
				checkAuthority();
			const auto &tmp = stage;
			if (unlinkat(directory.fd, tmp.c_str(), 0) < 0 && errno != ENOENT)
				fail("discard previous native graph stage");
			File f(openat(directory.fd, tmp.c_str(), O_CREAT | O_EXCL | (name == main ? O_RDWR : O_WRONLY) | O_CLOEXEC | O_NOFOLLOW, 0600));
			bool renamed = false;
			try {
				emit(f.fd);
				syncFile(f.fd);
				// Lock the replacement before it can be opened through the official
				// name. A duplicate keeps the same open file description locked
				// across the checked close, rename and directory fence.
				std::shared_ptr<File> successor;
				if (name == main) {
					File duplicate(fcntl(f.fd, F_DUPFD_CLOEXEC, 0));
					lockFile(duplicate.fd, false);
					successor = std::make_shared<File>(std::move(duplicate));
				}
				if (::close(std::exchange(f.fd, -1)) < 0)
					fail("close native graph stage");
				if (beforePublish)
					beforePublish();
				if (renameat(directory.fd, tmp.c_str(), directory.fd, name.c_str()) < 0)
					fail("install native graph stage");
				renamed = true;
				if (successor)
					authority = std::move(successor);
				syncFile(directory.fd);
				if (name == log)
					activeLog = std::make_unique<File>(
							File(openat(directory.fd, log.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC)));
			} catch (...) {
				if (!renamed)
					unlinkat(directory.fd, tmp.c_str(), 0);
				throw;
			}
		}
		void install(const std::string &name, Span b, size_t padding = 0) {
			installStream(name, [&](int fd) {
				writeComplete(fd, b);
				emitPadding(padding, [&](Span chunk) { writeComplete(fd, chunk); });
			});
		}
		Digest installBase(State &s, const std::function<void()> &beforePublish = {}, bool rooted = false) {
			utils::Sha256 hash;
			struct stat written{};
			CheckpointLocations nodes, edges;
			std::optional<EmbeddedRoot> prepared;
			size_t nodeStart = 0, edgeStart = 0;
			installStream(
					main,
					[&](int fd) {
						if (rooted) {
							emitPadding(baseStart, [&](Span data) { writeComplete(fd, data); });
							const std::string_view tag = "ZYXrt002";
							putAt(fd, 0, std::as_bytes(std::span(tag)));
						}
						// Coalesce small rows while hashing the exact independent main image.
						auto window = std::min(size_t(16384), s.budget->capacity() / 16);
						VersionCharge buffering(s.budget, window);
						Encoder buffer(window);
						buffer.bytes.reserve(window);
						auto flush = [&] {
							if (!buffer.bytes.empty())
								writeComplete(fd, buffer.bytes);
							buffer.bytes.clear();
						};
						auto put = [&](Span b) {
							hash.update(b);
							if (b.size() > buffer.limit - buffer.bytes.size())
								flush();
							if (b.size() > buffer.limit)
								writeComplete(fd, b);
							else
								buffer.raw(b);
						};
						Encoder head(112);
						head.magic(signature(s));
						head.raw(s.identity);
						head.num(s.sequence);
						head.num(s.sequence);
						head.num(0);
						head.raw(digest({}));
						head.num(s.nodeHigh);
						head.num(s.edgeHigh);
						head.num(s.nodes.size());
						head.num(s.edges.size());
						put(head.bytes);
						VersionMemoryResource rowMemory(s.budget);
						RowEncoder encoded(s.budget->capacity(), s.budget, &rowMemory);
						nodeStart = head.bytes.size() + (rooted ? baseStart : 0);
						std::shared_ptr<IndexedColumn> projection;
						if (signature(s) == "ZYXng006") {
							Encoder record(16384);
							record.num(s.unique ? (s.unique->ownsProjection ? 3 : 2) : (s.declaration ? 1 : 0));
							record.num(s.catalog ? s.catalog->entries.size() : 0);
							put(record.bytes);
							nodeStart += record.bytes.size();
							if (s.catalog)
								for (const auto &entry: s.catalog->entries) {
									record.bytes.clear();
									record.text(entry.name);
									record.text(entry.label);
									record.text(entry.key);
									record.num(entry.revision);
									put(record.bytes);
									nodeStart += record.bytes.size();
								}
						}
						if (s.declaration) {
							Encoder names(16384);
							names.text(s.declaration->label);
							names.text(s.declaration->key);
							if (s.unique) {
								names.text(s.unique->name);
								names.num(s.unique->revision);
							}
							put(names.bytes);
							nodeStart += names.bytes.size();
							projection = std::make_shared<IndexedColumn>(s.budget, s.nodes.size());
						}
						size_t offset = nodeStart;
						auto rows = [&](const auto &table, CheckpointLocations &layout) {
							table.visitAfter(0, [&](auto it) {
								auto id = it.id();
								auto size = it.encodedBytes() - 16;
								require(size <= s.budget->capacity(), "Native checkpoint row exceeds version budget");
								// A large-file API can outgrow the in-memory offset type
								// on a 32-bit host. Include this header and the final digest.
								if constexpr (sizeof(off_t) > sizeof(size_t))
									require(offset <= SIZE_MAX - size - 16 - Digest{}.size(),
											"Native checkpoint exceeds addressable extent");
								if (size > encoded.bytes.capacity()) {
									// The previous buffer is empty and can be freed before growing.
									std::pmr::vector<std::byte>(&rowMemory).swap(encoded.bytes);
									encoded.bytes.reserve(size);
								}
								if (it.onDisk()) {
									encoded.bytes.resize(size);
									it.readImage(encoded.bytes);
									layout.observe(id, it.offset(), offset + 16);
								} else {
									encodeBaseRow(encoded, it->second);
								}
								if constexpr (std::is_same_v<typename std::decay_t<decltype(table)>::mapped_type,
															 LogicalGraphNode>) {
									if (projection) {
										VersionCharge decode(s.budget, 0);
										projection->observe(*s.declaration, baseNode(id, encoded.bytes, decode));
									}
								}
								offset += size + 16;
								require(encoded.bytes.size() == size, "Native checkpoint size accounting mismatch");
								std::array<std::byte, 16> h{};
								for (unsigned i = 0; i < 8; ++i) {
									h[i] = std::byte(id >> (8 * i));
									h[8 + i] = std::byte(uint64_t(size) >> (8 * i));
								}
								put(h);
								put(encoded.bytes);
								encoded.bytes.clear();
								return true;
							});
						};
						rows(s.nodes, nodes);
						edgeStart = offset;
						rows(s.edges, edges);
						if (projection) {
							projection->buildPostings();
							Encoder field(16);
							auto pair = [&](uint64_t a, uint64_t b) {
								field.bytes.clear();
								field.num(a);
								field.num(b);
								put(field.bytes);
							};
							pair(projection->column.size(), projection->complete);
							for (auto [id, v]: projection->column)
								pair(id, std::bit_cast<uint64_t>(v));
							for (auto [v, id]: projection->postings)
								pair(std::bit_cast<uint64_t>(v), id);
							projection->releaseUnusablePostings();
							s.column = std::move(projection);
							s.changedColumn.reset();
							s.columnOverflow = false;
						}
						flush();
						writeComplete(fd, hash.finalize());
						if (rooted) {
							auto end = static_cast<size_t>(metadata(fd).st_size);
							prepared =
									EmbeddedRoot{s.identity, end, end, s.sequence, hash.finalize(), hash.finalize(), 0};
							auto record = prepared->encode(), ack = receipt(*prepared, record);
							putAt(fd, 2 * rootPage, record);
							putAt(fd, receipt1, ack);
							putAt(fd, receipt2, ack);
						}
						written = metadata(fd);
					},
					beforePublish);
			auto [published, extent] = openInput(main);
			auto st = metadata(published->fd);
			require(sameContentStamp(written, st), "Published native base identity changed");
			remember(published, st);
			root = prepared;
			nodes.bind(s.nodes, published, nodeStart, s.budget);
			edges.bind(s.edges, published, edgeStart, s.budget);
			return hash.finalize();
		}
		void discardLegacyStage(const Identity &id) {
			// Old experimental builds used the 16-byte graph ID alone. Only
			// check after authenticating it. The shared directory flock excludes
			// legacy writers, and current stage names use disjoint 32-byte keys.
			auto old = stageName(id);
			if (exists(old)) {
				if (unlinkat(directory.fd, old.c_str(), 0) < 0 && errno != ENOENT)
					fail("remove abandoned legacy native stage");
				syncFile(directory.fd);
			}
		}
		void removeWal() {
			activeLog.reset();
			for (const auto &name: {log, anchor()}) {
				if (unlinkat(directory.fd, name.c_str(), 0) < 0) {
					if (errno == ENOENT)
						continue;
					fail("remove native graph dependency");
				}
				syncFile(directory.fd);
			}
		}
	};
	std::string Journal::create(const std::string &name, std::shared_ptr<const ColumnDeclaration> declaration) {
		std::filesystem::path target(name);
		require(!target.filename().empty() && target.filename() != "." && target.filename() != "..",
				"Invalid native graph directory");
		File parent(open((target.has_parent_path() ? target.parent_path() : ".").c_str(),
						 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		if (mkdirat(parent.fd, target.filename().c_str(), 0700) < 0)
			fail("exclusively create native graph directory");
		syncFile(parent.fd);
		auto path = target / "main";
		Files files(path);
		State s;
		s.identity = identity();
		s.declaration = std::move(declaration);
		files.installBase(s);
		return path.string();
	}
	Journal::Journal(const std::string &path, bool ro, size_t memory) :
		files(std::make_unique<Files>(path)), readOnly(ro) {
		recover(std::make_shared<State>(memory), nullptr);
	}
	Journal::Journal(std::unique_ptr<Files> inspection) : files(std::move(inspection)), readOnly(true) {}
	void Journal::recover(std::shared_ptr<State> s, NativeWalIntegrityReport *audit, const State *retained) {
		const auto memory = s->budget->capacity();
		auto [main, extent] = audit ? files->openInput(files->main) : files->lockMain(readOnly);
		struct CheckedFile {
			std::shared_ptr<File> file;
			struct stat stamp;
		};
		std::optional<CheckedFile> walInput, baseInput;
		require(extent >= 112, "Incomplete native graph main");
		auto initial = Files::metadata(main->fd);
		std::array<std::byte, 8> tag{};
		readExact(main->fd, 0, tag);
		bool repairReceipts = false;
		if (std::memcmp(tag.data(), "ZYXrt002", 8) == 0)
			files->root = readRoot(main->fd, extent, repairReceipts);
		auto root = files->root;
		Cursor m(main->fd, root ? root->baseEnd : extent, s->budget, memory, root ? baseStart : 0);
		auto head = m.fixed<80>();
		Decoder r{head};
		const bool catalogFormat = std::memcmp(head.data(), "ZYXng006", 8) == 0;
		bool unique = std::memcmp(head.data(), "ZYXng005", 8) == 0;
		bool indexed = unique || std::memcmp(head.data(), "ZYXng004", 8) == 0;
		r.magic(catalogFormat ? "ZYXng006" : unique ? "ZYXng005" : indexed ? "ZYXng004" : "ZYXng003");
		std::ranges::copy(r.take(16), s->identity.begin());
		baseSequence = r.num();
		auto sequence = r.num();
		auto needed = r.count(SIZE_MAX);
		auto expected = r.take(32);
		require(baseSequence <= sequence && ((baseSequence == sequence) == (needed == 0)),
				"Invalid native graph receipt");
		s->sequence = baseSequence;
		auto finish = [](Cursor &c) {
			auto hash = c.hash.finalize();
			auto tail = c.fixed<32>();
			require(!c.remaining() && hash == tail, "Native graph main checksum mismatch");
			c.suspend();
			return hash;
		};
		auto loadBase = [&](Cursor &c, const std::shared_ptr<File> &file) {
			s->nodeHigh = c.num();
			s->edgeHigh = c.num();
			require(s->nodeHigh <= INT64_MAX && s->edgeHigh <= INT64_MAX, "Invalid native base ID high-water mark");
			auto nodes = c.count(c.remaining() / 16), edges = c.count(c.remaining() / 16);
			std::shared_ptr<IndexedColumn> projection;
			auto name = [&](size_t limit) {
				auto n = c.count(limit);
				require(n > 0, "Empty native schema name");
				std::string value(n, char{});
				c.consume(std::as_writable_bytes(std::span(value)));
				return value;
			};
			bool ownsProjection = false;
			if (catalogFormat) {
				const auto mode = c.count(3);
				indexed = mode >= 1;
				unique = mode >= 2;
				ownsProjection = mode == 3;
				const auto count = c.count(UniqueCatalog::maximum - size_t(unique));
				require(count <= c.remaining() / 35, "Invalid native schema extent");
				if (count) {
					auto catalog = std::make_shared<UniqueCatalog>(s->budget, count);
					for (size_t i = 0; i < count; ++i) {
						ScopedUnique entry{name(4096), name(4096), name(1024), c.num()};
						require(entry.revision && entry.revision <= baseSequence &&
										(!i || entry.revision > catalog->entries.back().revision),
								"Invalid native schema revision");
						catalog->append(entry);
					}
					s->catalog = std::move(catalog);
				}
			}
			if (indexed) {
				s->declaration = std::make_shared<ColumnDeclaration>(ColumnDeclaration{name(4096), name(1024)});
				if (unique) {
					auto constraint = name(4096);
					auto revision = c.num();
					require(revision && revision <= baseSequence, "Invalid unique declaration revision");
					s->unique = std::make_shared<UniqueDeclaration>(
							UniqueDeclaration{std::move(constraint), revision, ownsProjection});
				}
				projection = std::make_shared<IndexedColumn>(s->budget, nodes);
			}
			// A subsequent receipt must select the same format as its sealed
			// anchor, including when the catalog has just one long durable name.
			require(signature(*s) == std::string_view(reinterpret_cast<const char *>(head.data()), 8),
					"Noncanonical native schema header");
			const bool lazy = c.remaining() > std::min(size_t(256 * 1024), memory / 2);
			auto rows = [&](size_t n, uint64_t high, auto &table, auto decode, auto inspect, auto previousTable) {
				uint64_t prior = 0;
				table.loadSorted(n, [&](const auto &previous) {
					auto id = c.num();
					auto size = c.count(memory);
					require(id > prior && id <= high && size <= c.remaining(), "Invalid native base row extent");
					prior = id;
					const bool compact = audit && size > s->budget->remaining() / 2;
					VersionCharge wire(s->budget, size);
					VersionCharge scratch(s->budget, 0);
					auto offset = c.position;
					Bytes b(size);
					c.consume(b);
					auto row = compact ? inspect(id, b, scratch, *s) : decode(id, b, scratch);
					if constexpr (std::is_same_v<decltype(row), LogicalGraphNode>)
						if (projection)
							projection->observe(*s->declaration, row);
					if constexpr (std::is_same_v<decltype(row), LogicalGraphEdge>)
						require(s->nodes.contains(row.source_node_id) && s->nodes.contains(row.target_node_id),
								"Native WAL dangling relationship");
					if (!lazy || compact)
						return table.residentRecord(id, std::move(row), scratch);
					return table.baseRecord(id, size + 16, offset, digest(b), decode, previous);
				}, previousTable);
				table.bindInput(file);
			};
			rows(nodes, s->nodeHigh, s->nodes, baseNode, inspectBaseNode, retained ? &retained->nodes : nullptr);
			rows(edges, s->edgeHigh, s->edges, baseEdge, inspectBaseEdge, retained ? &retained->edges : nullptr);
			if (projection) {
				projection->buildPostings();
				require(c.num() == projection->column.size() && c.num() == projection->complete,
						"Invalid native projection coverage");
				for (auto [id, v]: projection->column)
					require(c.num() == id && c.num() == std::bit_cast<uint64_t>(v), "Native projection mismatch");
				for (auto [v, id]: projection->postings)
					require(c.num() == std::bit_cast<uint64_t>(v) && c.num() == id, "Native posting mismatch");
				projection->releaseUnusablePostings();
				s->column = std::move(projection);
			}
			if (audit)
				audit->base_generation = s->sequence;
			baseDigest = finish(c);
			if (s->catalog) {
				VersionCharge audit(s->budget, (s->catalog->entries.size() + 1) * 256);
				std::set<std::string_view> names;
				std::set<std::pair<std::string_view, std::string_view>> scopes;
				std::set<uint64_t> revisions;
				if (s->unique) {
					names.insert(s->unique->name);
					scopes.emplace(s->declaration->label, s->declaration->key);
					revisions.insert(s->unique->revision);
				}
				for (const auto &entry: s->catalog->entries)
					require(names.insert(entry.name).second && scopes.emplace(entry.label, entry.key).second &&
									revisions.insert(entry.revision).second,
							"Duplicate native schema declaration");
				validateCatalog(*s);
			}
			// Every posting was reconstructed from its row and matched to the
			// sealed base. With complete integer coverage, adjacent keys are
			// an exhaustive uniqueness audit, including an empty declaration.
			if (s->unique && s->column && s->column->complete) {
				const auto &posts = s->column->postings;
				if (std::ranges::adjacent_find(posts, {}, &Posting::first) != posts.end())
					throw UniqueViolation("Constraint violation: '" + s->unique->name + "' has a duplicate key");
			} else
				validateUnique(*s);
			uniqueOwners.bind(*s);
		};
		// The ordinary WAL binds the logical head of this entire authenticated
		// base, which can itself include confirmed in-file transactions.
		auto replayEmbedded = [&](const EmbeddedRoot &root, const File &source) {
			require(baseDigest == root.base, "Invalid embedded base digest");
			Cursor delta(source.fd, root.end, s->budget, memory, root.baseEnd);
			auto chain = baseDigest;
			while (delta.remaining()) {
				delta.hash = utils::Sha256{};
				delta.hash.update(chain);
				auto header = delta.fixed<24>();
				Decoder tx{header};
				tx.magic("ZYXtx002");
				require(s->sequence < UINT64_MAX && tx.num() == s->sequence + 1, "Embedded transaction gap");
				auto n = tx.count(delta.remaining());
				UniqueDelta check(*s, uniqueOwners);
				applyDelta(*s, n, [&](std::span<std::byte> part) { delta.consume(part); }, check, audit != nullptr);
				++s->sequence;
				if (audit)
					++audit->embedded_transactions;
				check.advance(*s);
				chain = delta.hash.finalize();
			}
			require(chain == root.chain && s->sequence == root.generation, "Invalid embedded transaction chain");
			baseDigest = root.chain;
			baseSequence = root.generation;
		};
		require(!root || (!needed && root->id == s->identity && sequence <= root->generation),
				"Invalid native root identity or revision");
		if (!needed) {
			require(std::ranges::equal(digest({}), expected), "Invalid standalone digest");
			loadBase(m, main);
			if (root)
				replayEmbedded(*root, *main);
			files->remember(main, initial);
			needsStabilization = repairReceipts || files->exists(files->anchor()) || files->exists(files->log) ||
								 files->exists(files->stage);
		} else {
			// A seal receipt pins the old immutable extent independently of the
			// anchor control pages while they are being prepared for promotion.
			std::optional<std::array<std::byte, 144>> sealBase;
			if (m.remaining() == 144 + 32)
				sealBase = m.fixed<144>();
			finish(m);
			auto [wal, length] = files->openInput(files->log);
			if (audit)
				walInput.emplace(CheckedFile{wal, Files::metadata(wal->fd)});
			require(length >= needed, "Missing native graph WAL prefix");
			Cursor log(wal->fd, needed, s->budget, memory);
			// A receipt fitting one admitted window can be replayed directly
			// from those authenticated bytes. Larger WALs rehash during replay.
			log.authenticateWal(expected);
			auto wh = log.fixed<72>(false);
			log.suspend();
			Decoder h{wh};
			h.magic("ZYXnw003");
			require(std::ranges::equal(s->identity, h.take(16)) && h.num() == baseSequence,
					"Native graph WAL generation mismatch");
			auto alignment = h.count(65536);
			require(alignment >= 4096 && (alignment & (alignment - 1)) == 0, "Invalid native WAL alignment");
			auto anchor = files->openInput(files->anchor());
			// A required WAL binds one immutable anchor. The sealed logical
			// head is authenticated before any external transactions are replayed.
			std::array<std::byte, 8> anchorTag{};
			readExact(anchor.first->fd, 0, anchorTag);
			std::optional<EmbeddedRoot> rootedAnchor;
			auto beforeAnchor = Files::metadata(anchor.first->fd);
			if (audit)
				baseInput.emplace(CheckedFile{anchor.first, beforeAnchor});
			if (sealBase) {
				require(std::memcmp(anchorTag.data(), "ZYXrt002", 8) == 0, "Invalid seal dependency");
				rootedAnchor = decodeRoot(*sealBase, anchor.second);
				require(rootedAnchor->id == s->identity && rootedAnchor->generation == baseSequence,
						"Invalid seal dependency identity");
			} else if (std::memcmp(anchorTag.data(), "ZYXrt002", 8) == 0) {
				bool repair = false;
				rootedAnchor = readRoot(anchor.first->fd, anchor.second, repair);
				// The external WAL supplies another authenticated binding to this
				// exact generation and chain digest. A single damaged or stale
				// redundant receipt need not make an acknowledged WAL unreadable.
				require(rootedAnchor->id == s->identity && rootedAnchor->generation == baseSequence,
						"Invalid rooted native WAL dependency");
			}
			Cursor a(anchor.first->fd, rootedAnchor ? rootedAnchor->baseEnd : anchor.second, s->budget, memory,
					 rootedAnchor ? baseStart : 0);
			auto ah = a.fixed<80>();
			Decoder ar{ah};
			ar.magic(catalogFormat ? "ZYXng006" : unique ? "ZYXng005" : indexed ? "ZYXng004" : "ZYXng003");
			require(std::ranges::equal(ar.take(16), s->identity), "Native graph base generation mismatch");
			auto origin = ar.num();
			require((rootedAnchor ? origin <= baseSequence : origin == baseSequence) && ar.num() == origin &&
							ar.num() == 0 && std::ranges::equal(ar.take(32), digest({})),
					"Native graph base generation mismatch");
			baseSequence = origin;
			s->sequence = origin;
			loadBase(a, anchor.first);
			if (rootedAnchor)
				replayEmbedded(*rootedAnchor, *anchor.first);
			require(std::ranges::equal(h.take(32), baseDigest), "Native graph base digest mismatch");
			while (log.remaining()) {
				auto header = log.fixed<24>();
				Decoder tx{header};
				tx.magic("ZYXtx002");
				auto seq = tx.num();
				require(s->sequence < UINT64_MAX && seq == s->sequence + 1, "Native graph WAL sequence gap");
				auto size = tx.count(log.remaining());
				UniqueDelta check(*s, uniqueOwners);
				applyDelta(*s, size, [&](std::span<std::byte> bytes) { log.consume(bytes); }, check, audit != nullptr);
				s->sequence = seq;
				if (audit)
					++audit->wal_transactions;
				check.advance(*s);
				size_t padding = (alignment - log.position % alignment) % alignment;
				std::array<std::byte, 512> zeros;
				while (padding) {
					auto n = std::min(padding, zeros.size());
					auto out = std::span(zeros).first(n);
					log.consume(out);
					require(std::ranges::all_of(out, [](auto b) { return b == std::byte{}; }),
							"Invalid native WAL frame padding");
					padding -= n;
				}
			}
			walHash = log.hash;
			walLength = needed;
			if (!sealBase && rootedAnchor)
				files->remember(anchor.first, beforeAnchor);
			require(std::ranges::equal(walHash.finalize(), expected) && s->sequence == sequence,
					"Incomplete native graph transaction");
		}
		if (audit) {
			// Check the final merged graph, including survival of endpoints
			// across separate WAL transactions and the high-water marks.
			s->nodes.visitAfter(0, [&](const auto &it) {
				require(it.id() <= s->nodeHigh && (it.onDisk() || it->second.node_id == it.id()), "Invalid verified node identity");
				++audit->nodes;
				return true;
			});
			for (const auto &[id, row] : s->edges) {
				require(id && id <= s->edgeHigh && row.edge_id == id && s->nodes.contains(row.source_node_id) &&
								s->nodes.contains(row.target_node_id), "Invalid verified edge or endpoint");
				++audit->edges;
			}
			auto unchanged = [&](const std::string &name, const CheckedFile &input) {
				struct stat named{};
				if (fstatat(files->directory.fd, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
					fail("stat verified native input");
				require(Files::sameStamp(input.stamp, named) &&
								Files::sameStamp(input.stamp, Files::metadata(input.file->fd)),
						"Native audit input changed");
			};
			unchanged(files->main, CheckedFile{main, initial});
			if (walInput) unchanged(files->log, *walInput);
			if (baseInput) unchanged(files->anchor(), *baseInput);
			audit->generation = s->sequence;
			audit->confirmed_wal_bytes = walLength;
			audit->unique_constraints = size_t(bool(s->unique)) + (s->catalog ? s->catalog->entries.size() : 0);
			audit->has_declared_column = bool(s->declaration);
			audit->base_column_complete = s->column && s->column->complete;
		}
		if (!readOnly)
			files->discardLegacyStage(s->identity);
		current = std::move(s);
	}
	NativeWalIntegrityReport Journal::verifyIntegrity(size_t workspace) {
		std::lock_guard lock(gate);
		require(!poisoned, "Native graph requires recovery");
		NativeWalIntegrityReport report;
		try {
			files->checkAuthority();
			Journal inspected(std::make_unique<Files>(*files));
			auto budget = std::make_shared<VersionBudget>(current->budget, workspace);
			inspected.recover(std::make_shared<State>(std::move(budget)), &report, current.get());
			const auto &view = *inspected.current;
			require(view.identity == current->identity && view.sequence == current->sequence &&
							view.nodeHigh == current->nodeHigh && view.edgeHigh == current->edgeHigh &&
							report.nodes == current->nodes.size() && report.edges == current->edges.size() &&
							baseSequence == inspected.baseSequence && baseDigest == inspected.baseDigest &&
							walLength == inspected.walLength && (!walLength || walHash.finalize() == inspected.walHash.finalize()),
					"Verified files do not match the confirmed native generation");
			files->checkAuthority();
		} catch (const std::system_error &) {
			throw;
		} catch (const FormatException &) {
			throw;
		} catch (const std::runtime_error &e) {
			throw FormatException(FormatError::FERR_INVALID_STATE, e.what());
		}
		return report;
	}
	Journal::~Journal() = default;
	std::shared_ptr<const State> Journal::snapshot() {
		std::lock_guard lock(gate);
		require(!poisoned, "Native graph requires recovery");
		return current;
	}
	void Journal::publishBase(std::shared_ptr<State> rebased, const std::function<void()> &beforePublish,
							  bool retireDependencies, bool rooted) {
		auto fresh = files->installBase(*rebased, beforePublish, rooted);
		if (retireDependencies)
			files->removeWal();
		current = std::move(rebased);
		uniqueOwners.bind(*current);
		baseDigest = fresh;
		baseSequence = current->sequence;
		walLength = 0;
		walHash = utils::Sha256{};
		needsStabilization = false;
	}
	void Journal::checkpointLocked() { publishBase(std::make_shared<State>(*current)); }
	uint64_t Journal::createUnique(const std::string &name, const std::string &label, const std::string &key,
								   const std::function<void()> &check, const std::function<void(size_t)> &admit) {
		std::lock_guard lock(gate);
		require(!readOnly && !poisoned, "Native schema writer unavailable");
		require(!name.empty() && name.size() <= 4096 && !key.empty() && key.size() <= 1024 && !label.empty() &&
						label.size() <= 4096,
				"Invalid native UNIQUE name or scope");
		const bool matchesColumn =
				current->declaration && current->declaration->label == label && current->declaration->key == key;
		const auto clashes = [&](const ScopedUnique &d) {
			return d.name == name || (d.label == label && d.key == key);
		};
		require(!(current->unique && (current->unique->name == name || matchesColumn)) &&
						(!current->catalog || std::ranges::none_of(current->catalog->entries, clashes)),
				"System Catalog constraint definition duplicates a durable name or schema");
		require(!current->catalog ||
						current->catalog->entries.size() + size_t(bool(current->unique)) < UniqueCatalog::maximum,
				"Native constraint catalog is full");
		require(current->sequence < UINT64_MAX, "Native graph sequence exhausted");
		auto next = std::make_shared<State>(*current);
		++next->sequence;
		ScopedUnique entry{name, label, key, next->sequence};
		validateScope(*next, entry, check, admit);
		if (!next->declaration || (matchesColumn && !next->unique)) {
			const bool ownsProjection = !next->declaration;
			if (ownsProjection)
				next->declaration = std::make_shared<ColumnDeclaration>(ColumnDeclaration{label, key});
			next->unique = std::make_shared<UniqueDeclaration>(UniqueDeclaration{name, next->sequence, ownsProjection});
		} else {
			size_t count = next->catalog ? next->catalog->entries.size() : 0;
			auto catalog = std::make_shared<UniqueCatalog>(next->budget, count + 1);
			if (next->catalog)
				for (const auto &existing: next->catalog->entries)
					catalog->append(existing);
			catalog->append(entry);
			next->catalog = std::move(catalog);
		}
		publishSchema(std::move(next), check);
		return current->sequence;
	}
	bool Journal::dropUnique(std::string_view name) {
		std::lock_guard lock(gate);
		require(!readOnly && !poisoned, "Native schema writer unavailable");
		const bool primary = current->unique && current->unique->name == name;
		if (!primary && (!current->catalog || std::ranges::find(current->catalog->entries, name, &ScopedUnique::name) ==
													  current->catalog->entries.end()))
			return false;
		require(current->sequence < UINT64_MAX, "Native graph sequence exhausted");
		auto next = std::make_shared<State>(*current);
		++next->sequence;
		if (primary && next->unique->ownsProjection) {
			next->declaration.reset();
			next->column.reset();
			next->changedColumn.reset();
			next->columnOverflow = false;
		}
		if (primary)
			next->unique.reset();
		else if (next->catalog->entries.size() == 1)
			next->catalog.reset();
		else {
			auto catalog = std::make_shared<UniqueCatalog>(next->budget, next->catalog->entries.size() - 1);
			for (const auto &existing: next->catalog->entries)
				if (existing.name != name)
					catalog->append(existing);
			next->catalog = std::move(catalog);
		}
		publishSchema(std::move(next), {});
		return true;
	}
	void Journal::publishSchema(std::shared_ptr<State> next, const std::function<void()> &check) {
		if (check)
			check();
		bool crossedPublicationBoundary = false;
		bool cancelledBeforeRename = false;
		auto beforePublish = [&] {
			try {
				if (check)
					check();
			} catch (...) {
				cancelledBeforeRename = true;
				throw;
			}
			crossedPublicationBoundary = true;
		};
		try {
			files->checkAuthority();
			publishBase(std::move(next), beforePublish);
		} catch (const std::length_error &) {
			poisoned = crossedPublicationBoundary;
			throw;
		} catch (const std::bad_alloc &) {
			poisoned = crossedPublicationBoundary;
			throw;
		} catch (...) {
			if (!cancelledBeforeRename)
				poisoned = true;
			throw;
		}
	}
	void Journal::checkpoint() {
		std::lock_guard lock(gate);
		if (readOnly)
			return;
		require(!poisoned, "Native graph requires recovery");
		try {
			if (needsStabilization || walLength != 0)
				checkpointLocked();
		} catch (...) {
			poisoned = true;
			throw;
		}
	}
	// Seal A independently of B so an interruption in dependency cleanup
	// cannot report B as failed after making B visible. The exact old root
	// is durable in main before the anchor's control area can be changed.
	bool Journal::sealWal() {
		if (!walLength || !files->stable)
			return false;
		files->checkAuthority();
		files->checkBaseName(false);
		const auto initial = Files::metadata(files->stable->fd);
		const auto receiptFile = Files::metadata(files->authority->fd);
		// Promoting an inode must preserve the private successor's access.
		// A readable dependency is sufficient for normal full replacement.
		if ((initial.st_mode & 07777) != 0600 || (receiptFile.st_mode & 07777) != 0600 ||
			initial.st_uid != receiptFile.st_uid || initial.st_gid != receiptFile.st_gid)
			return false;
		std::array<std::byte, 8> tag{};
		readExact(files->stable->fd, 0, tag);
		if (std::memcmp(tag.data(), "ZYXrt002", 8) != 0)
			return false;
		bool repair = false;
		auto root = readRoot(files->stable->fd, size_t(initial.st_size), repair);
		require(root.id == current->identity && root.generation == baseSequence && root.chain == baseDigest,
				"Native seal base mismatch");
		size_t limit = (root.baseEnd - baseStart) / 2, used = root.end - root.baseEnd;
		auto nodes = current->nodes.encodedBytes();
		if (repair || used > limit || walLength > limit - used ||
			(nodes < limit && current->edges.encodedBytes() < limit - nodes) ||
			walLength > size_t(INT64_MAX) - root.end)
			return false;
		auto [wal, extentWal] = files->openInput(files->log);
		require(extentWal >= walLength, "Missing native graph WAL prefix");
		auto wstamp = Files::metadata(wal->fd);
		auto budget = current->budget;
		Cursor input(wal->fd, walLength, budget, budget->capacity());
		auto expectedHash = walHash.finalize();
		auto header = input.fixed<72>();
		Decoder h{header};
		h.magic("ZYXnw003");
		require(std::ranges::equal(h.take(16), root.id) && h.num() == root.generation, "Native seal WAL generation");
		auto alignment = h.count(65536);
		require(alignment >= 4096 && !(alignment & (alignment - 1)) && std::ranges::equal(h.take(32), root.chain),
				"Invalid native seal WAL header");
		auto size = std::min({size_t(32768), budget->capacity() / 16, walLength - input.position});
		require(size > 0, "Empty native seal WAL");
		VersionCharge charge(budget, size);
		Bytes buffer(size);
		auto prior = root.encode();
		std::shared_ptr<File> anchor;
		try {
			anchor = files->openInput(files->anchor(), O_RDWR).first;
		} catch (const std::system_error &e) {
			if (e.code() == std::errc::permission_denied || e.code() == std::errc::read_only_file_system)
				return false;
			throw;
		}
		require(Files::sameStamp(Files::metadata(anchor->fd), initial), "Native seal anchor changed");
		auto flags = fcntl(files->stable->fd, F_GETFL);
		if (flags < 0)
			fail("inspect native anchor descriptor");
		if ((flags & O_ACCMODE) == O_RDWR)
			anchor = files->stable;
		Files::lockFile(anchor->fd, false);
		files->install(files->main, manifest(*current, baseSequence, prior, walLength, expectedHash));
		files->checkBaseName(false);
		while (input.remaining()) {
			auto frame = input.fixed<24>();
			Decoder tx{frame};
			tx.magic("ZYXtx002");
			require(root.generation < UINT64_MAX && tx.num() == ++root.generation, "Native seal sequence gap");
			auto length = tx.count(input.remaining());
			utils::Sha256 chain;
			chain.update(root.chain);
			auto put = [&](Span bytes) {
				chain.update(bytes);
				putAt(anchor->fd, root.end, bytes);
				root.end += bytes.size();
			};
			put(frame);
			while (length) {
				auto part = std::span(buffer).first(std::min(length, size));
				input.consume(part);
				put(part);
				length -= part.size();
			}
			root.chain = chain.finalize();
			size_t padding = (alignment - input.position % alignment) % alignment;
			while (padding) {
				auto part = std::span(buffer).first(std::min(size, padding));
				input.consume(part);
				require(std::ranges::all_of(part, [](auto b) { return b == std::byte{}; }), "Invalid WAL padding");
				padding -= part.size();
			}
		}
		require(root.generation == current->sequence && input.hash.finalize() == expectedHash &&
						Files::sameStamp(Files::metadata(wal->fd), wstamp), "Native seal receipt mismatch");
		root.slot ^= 1;
		auto record = root.encode(), ack = receipt(root, record);
		putAt(anchor->fd, (2 + root.slot) * rootPage, record);
		putAt(anchor->fd, receipt1, ack);
		putAt(anchor->fd, receipt2, ack);
		std::array<std::byte, 8> retired{};
		putAt(anchor->fd, (2 + (root.slot ^ 1)) * rootPage, retired);
		syncFile(anchor->fd);
		files->checkAuthority();
		struct stat named{};
		if (fstatat(files->directory.fd, files->anchor().c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
			fail("stat native seal anchor");
		require(named.st_nlink == 1 && named.st_dev == initial.st_dev && named.st_ino == initial.st_ino,
				"Native seal authority changed");
		if (renameat(files->directory.fd, files->anchor().c_str(), files->directory.fd, files->main.c_str()) < 0)
			fail("promote native seal");
		files->authority = anchor;
		syncFile(files->directory.fd);
		files->removeWal();
		files->stable = anchor;
		files->authenticated = Files::metadata(anchor->fd);
		files->root = root;
		baseSequence = root.generation;
		baseDigest = root.chain;
		walLength = 0;
		walHash = utils::Sha256{};
		needsStabilization = false;
		return true;
	}
	void Journal::commit(const std::shared_ptr<const State> &expected, std::shared_ptr<State> next,
						 const Changes &changed, bool standalone) {
		std::lock_guard lock(gate);
		require(!readOnly && !poisoned, "Native graph writer unavailable");
		// A checkpoint may replace the physical root at the same revision.
		// The budget is owned by exactly one open journal and shared by every
		// derived root; matching a copied file's identity alone is insufficient.
		if (current != expected &&
			(!expected || current->budget != expected->budget || current->sequence != expected->sequence))
			throw LogicalGraphMutationConflict("Stale native graph write snapshot");
		require(current->sequence < UINT64_MAX, "Native graph sequence exhausted");
		next->sequence = current->sequence + 1;
		UniqueDelta check(*current, uniqueOwners);
		if (next->unique) {
			check.reserve(changed.nodes.size());
			for (auto id: changed.nodes)
				check.observe(id, check.key(*current, id), check.key(*next, id));
		}
		if (!changed.nodes.empty() && (next->unique || next->catalog))
			check.validate(*next, current.get(), [&](const auto &visit) {
				for (auto id: changed.nodes)
					visit(id);
			});
		// Seal only the already acknowledged prefix. A cleanup failure at
		// this stage must not publish the new transaction. A successful seal
		// yields a standalone compact base suitable for the sparse path below.
		if (standalone && (needsStabilization || walLength)) {
			try {
				if (!sealWal())
					publishBase(std::make_shared<State>(*current), {}, true, true);
			} catch (...) {
				poisoned = true;
				throw;
			}
		}
		if (standalone && files->root && !needsStabilization && !walLength) {
			auto root = *files->root;
			size_t payload = deltaBytes(*next, changed);
			size_t limit = (root.baseEnd - baseStart) / 2;
			size_t used = root.end - root.baseEnd;
			// The immutable base can be mostly dead after a large delete or
			// a wide-to-narrow replacement even when the delta itself is tiny.
			const auto nodeBytes = next->nodes.encodedBytes(), edgeBytes = next->edges.encodedBytes();
			const bool fitsLiveSet = nodeBytes >= limit || edgeBytes >= limit - nodeBytes;
			if (fitsLiveSet && used <= limit && limit - used >= 24 && payload <= limit - used - 24 &&
				root.end <= size_t(INT64_MAX) - 24 - payload) {
				files->checkAuthority();
				require(Files::sameStamp(Files::metadata(files->authority->fd), files->authenticated),
						"Embedded base changed before append");
				if (current != expected) {
					next->nodes.inheritInput(current->nodes);
					next->edges.inheritInput(current->edges);
				}
				next->column = current->column;
				next->changedColumn = current->changedColumn;
				next->columnOverflow = current->columnOverflow;
				next->recordColumnChanges(changed.nodes);
				auto fd = files->authority->fd;
				Encoder header(24);
				header.magic("ZYXtx002");
				header.num(next->sequence);
				header.num(payload);
				utils::Sha256 hash;
				hash.update(root.chain);
				auto window = std::min({size_t(32768), next->budget->capacity() / 16, 24 + payload});
				require(window > 0, "Empty embedded output window");
				VersionCharge charge(next->budget, window);
				Bytes buffer;
				buffer.reserve(window);
				size_t end = root.end;
				auto flush = [&] {
					putAt(fd, end, buffer);
					end += buffer.size();
					buffer.clear();
				};
				bool confirming = false;
				auto emit = [&](Span data) {
					hash.update(data);
					while (!data.empty()) {
						auto part = data.first(std::min(data.size(), window - buffer.size()));
						buffer.insert(buffer.end(), part.begin(), part.end());
						data = data.subspan(part.size());
						if (buffer.size() == window)
							flush();
					}
				};
				try {
					emit(header.bytes);
					writeDelta(*next, changed, emit);
					flush();
					require(end - root.end == 24 + payload, "Embedded length mismatch");
					root.end = end;
					root.generation = next->sequence;
					root.chain = hash.finalize();
					root.slot ^= 1;
					auto record = root.encode(), ack = receipt(root, record);
					putAt(fd, (2 + root.slot) * rootPage, record);
					syncFile(fd);
					confirming = true;
					putAt(fd, receipt1, ack);
					syncFile(fd);
					putAt(fd, receipt2, ack);
					syncFile(fd);
					// Neither durable receipt selects the predecessor now. Readers
					// pin immutable data extents separately from this control slot.
					// Retire its signature before ACK so a replay of both old
					// receipts cannot silently select a previously confirmed root.
					std::array<std::byte, 8> retired{};
					putAt(fd, (2 + (root.slot ^ 1)) * rootPage, retired);
					syncFile(fd);
					files->authenticated = Files::metadata(fd);
					files->root = root;
					baseSequence = root.generation;
					baseDigest = root.chain;
					check.advance(*next);
					current = std::move(next);
				} catch (...) {
					poisoned = true;
					if (confirming)
						std::throw_with_nested(
								std::runtime_error("Native bulk publication outcome indeterminate; reopen to recover"));
					throw;
				}
				return;
			}
		}
		if (standalone) {
			bool publishing = false;
			try {
				files->checkAuthority();
				if (current != expected) {
					next->nodes.inheritInput(current->nodes);
					next->edges.inheritInput(current->edges);
				}
				publishBase(std::move(next), [&] { publishing = true; }, false, true);
			} catch (...) {
				if (publishing) {
					poisoned = true;
					std::throw_with_nested(std::runtime_error(
							"Native bulk publication outcome indeterminate; reopen to recover"));
				}
				throw;
			}
			return;
		}
		const size_t payload = deltaBytes(*next, changed);
		const size_t fileLimit = std::min<uintmax_t>(SIZE_MAX, std::numeric_limits<off_t>::max());
		require(payload <= fileLimit - 96 - files->alignment, "Native WAL transaction extent overflow");
		const bool rotate = needsStabilization || (walLength &&
				(walLength >= maxWal || payload + 24 + files->alignment - 1 > maxWal - walLength));
		const size_t header = (rotate || !walLength ? 72 : 0) + 24;
		const size_t unpadded = header + payload;
		const size_t frameSize = unpadded + (files->alignment - unpadded % files->alignment) % files->alignment;
		// Small commits retain their single-write path. Large commits stage
		// only an admitted output window and the row currently being encoded.
		const bool streamed = frameSize > 32768;
		const size_t window = streamed ? std::min(size_t(32768), next->budget->capacity() / 16) : unpadded;
		require(window > 0, "Native WAL output window budget exceeded");
		VersionCharge frameCharge(next->budget, window);
		size_t resident = window;
		if (!streamed) {
			try {
				frameCharge.growTo(frameSize);
				resident = frameSize;
			} catch (const std::length_error &) {
				// The authenticated zero suffix can be emitted separately.
			}
		}
		Encoder frame(resident, next->budget);
		frame.bytes.reserve(resident);
		if (!streamed) {
			frame.bytes.resize(header);
			encodeDelta(frame, *next, changed);
			require(frame.bytes.size() == unpadded, "Native WAL measured delta size mismatch");
			frame.bytes.resize(resident);
		}
		try {
			files->checkAuthority();
			if (rotate)
				checkpointLocked();
			// Unchanged entries in this private transaction still carry the
			// original offsets. Inherit the locator map published for that
			// logical version, even if those bytes moved within the new file.
			if (current != expected) {
				next->nodes.inheritInput(current->nodes);
				next->edges.inheritInput(current->edges);
			}
			// The checkpoint may have replaced the base underlying the writer.
			next->column = current->column;
			next->changedColumn = current->changedColumn;
			next->columnOverflow = current->columnOverflow;
			next->recordColumnChanges(changed.nodes);
			Encoder bytes(header);
			if (walLength == 0) {
				bytes.magic("ZYXnw003");
				bytes.raw(next->identity);
				bytes.num(baseSequence);
				bytes.num(files->alignment);
				bytes.raw(baseDigest);
			}
			bytes.magic("ZYXtx002");
			bytes.num(next->sequence);
			bytes.num(payload);
			require(bytes.bytes.size() == header, "Native WAL generation header mismatch");
			auto hash = walHash;
			auto output = [&](int fd) {
				auto put = [&](Span data) {
					hash.update(data);
					Files::writeComplete(fd, data);
				};
				if (!streamed) {
					std::copy(bytes.bytes.begin(), bytes.bytes.end(), frame.bytes.begin());
					put(frame.bytes);
					Files::emitPadding(frameSize - resident, put);
					return;
				}
				size_t emitted = 0;
				auto buffer = [&](Span data) {
					require(data.size() <= frameSize - emitted, "Native WAL frame overflow");
					emitted += data.size();
					while (!data.empty()) {
						auto part = data.first(std::min(data.size(), resident - frame.bytes.size()));
						frame.raw(part);
						data = data.subspan(part.size());
						if (frame.bytes.size() == resident) {
							put(frame.bytes);
							frame.bytes.clear();
						}
					}
				};
				buffer(bytes.bytes);
				writeDelta(*next, changed, buffer);
				require(emitted == unpadded, "Native WAL measured delta size mismatch");
				Files::emitPadding(frameSize - unpadded, buffer);
				if (!frame.bytes.empty())
					put(frame.bytes);
			};
			if (!walLength) {
				files->retainBase();
				files->installStream(files->log, output);
			} else
				files->appendStream(walLength, output);
			const auto length = walLength + frameSize;
			auto receipt = manifest(*next, baseSequence, {}, length, hash.finalize());
			files->install(files->main, receipt);
			if (!walLength)
				files->checkBaseName(true);
			check.advance(*next);
			files->root.reset();
			walLength = length;
			walHash = hash;
			current = std::move(next);
		} catch (...) {
			poisoned = true;
			throw;
		}
	}
} // namespace graph::storage::nativewal
#else
namespace graph::storage::nativewal {
	struct Journal::Files {};
	namespace {
		[[noreturn]] void unavailable() {
			throw std::runtime_error("Experimental native WAL requires Linux local file operations");
		}
	} // namespace
	std::string Journal::create(const std::string &, std::shared_ptr<const ColumnDeclaration>) { unavailable(); }
	Journal::Journal(const std::string &, bool, size_t) { unavailable(); }
	NativeWalIntegrityReport Journal::verifyIntegrity(size_t workspace) { unavailable(); }
	Journal::~Journal() = default;
	std::shared_ptr<const State> Journal::snapshot() { unavailable(); }
	void Journal::commit(const std::shared_ptr<const State> &, std::shared_ptr<State>, const Changes &, bool) {
		unavailable();
	}
	void Journal::checkpoint() { unavailable(); }
	bool Journal::dropUnique(std::string_view) { unavailable(); }
	uint64_t Journal::createUnique(const std::string &, const std::string &, const std::string &,
								   const std::function<void()> &, const std::function<void(size_t)> &) {
		unavailable();
	}
} // namespace graph::storage::nativewal
#endif
