#include "Journal.hpp"
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
			void read(uint64_t, size_t offset, std::span<std::byte> bytes) const override { readExact(fd, offset, bytes); }
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
			bool startsRun(size_t from, size_t to) const {
				return from - original != to - published;
			}
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
		std::string stageName(const Identity &id) {
			const char *digits = "0123456789abcdef";
			std::string s = ".zyx-native-";
			for (auto b: id) {
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
			size_t extent, filled = 0, next = 0;
			VersionCharge admission;
			Bytes window;
			bool authenticatedWindow = false;

		public:
			utils::Sha256 hash;
			size_t position = 0;
			Cursor(int f, size_t end, std::shared_ptr<VersionBudget> budget, size_t memory) :
				fd(f), extent(end), admission(budget, std::min({size_t(32768), end, memory / 16})),
				window(std::min({size_t(32768), end, memory / 16})) {
				require(!window.empty(), "Native recovery window budget exceeded");
			}
			size_t remaining() const { return extent - position; }
			void consume(std::span<std::byte> out) {
				require(out.size() <= remaining(), "Truncated native graph field");
				while (!out.empty()) {
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
					filled = 0;
					hash = utils::Sha256{};
				}
			}
			template<size_t N>
			auto fixed() {
				std::array<std::byte, N> b;
				consume(b);
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
		Bytes manifest(const State &s, uint64_t baseSequence, Span base, size_t length, const Digest &hash) {
			Encoder w(maxState + 256);
			w.magic("ZYXng003");
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
		std::string main, log, stage;
		size_t alignment = 0;
		Files(const std::filesystem::path &path, bool ro) :
			directory(::open((path.has_parent_path() ? path.parent_path() : ".").c_str(),
							 O_DIRECTORY | O_RDONLY | O_CLOEXEC)),
			main(path.filename().string()), log(main + ".wal") {
			if (flock(directory.fd, (ro ? LOCK_SH : LOCK_EX) | LOCK_NB) < 0)
				fail("lock native graph directory");
			// Only the official main entry can publish. An anchored checkpoint is
			// itself a valid standalone image; opening its *managed name* for an
			// independent transaction would otherwise overwrite the reachable base.
			require(!main.starts_with(".zyx-native-"), "Managed native stage is not a database entry point");
			if (main.ends_with(".base")) {
				auto parent = main.substr(0, main.size() - 5);
				require(parent.empty() || (!exists(parent) && !exists(parent + ".wal")),
						"Managed native base is not a database entry point");
			}
			struct stat info{};
			if (fstat(directory.fd, &info) < 0)
				fail("stat native graph directory");
			auto page = sysconf(_SC_PAGESIZE);
			require(page > 0 && info.st_blksize > 0, "Unknown filesystem or VM block size");
			alignment = static_cast<size_t>(std::max({long(4096), page, static_cast<long>(info.st_blksize)}));
			require(alignment <= 65536 && (alignment & (alignment - 1)) == 0, "Unsupported native WAL block alignment");
		}
		std::string anchor() const { return main + ".base"; }
		bool exists(const std::string &name) const {
			struct stat st{};
			if (fstatat(directory.fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0)
				return true;
			if (errno == ENOENT)
				return false;
			fail("stat native dependency");
		}
		std::pair<std::shared_ptr<File>, size_t> openInput(const std::string &name) {
			auto fd = openat(directory.fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
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
		// the same sealed base through the directory locked by this writer.
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
				auto n = ::write(fd, bytes.data(), bytes.size());
				if (n < 0 && errno == EINTR)
					continue;
				if (n < 0)
					fail("write native graph file");
				require(n > 0, "Zero native graph write");
				bytes = bytes.subspan(n);
			}
		}
		std::unique_ptr<File> activeLog;
		void append(Span bytes, size_t offset) {
			require(bool(activeLog), "Missing native append authority");
			checkBaseName(false);
			struct stat st{}, named{};
			if (fstat(activeLog->fd, &st) < 0 || fstatat(directory.fd, log.c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
				fail("stat native append target");
			require(st.st_nlink == 1 && named.st_dev == st.st_dev && named.st_ino == st.st_ino && st.st_size >= 0 &&
							static_cast<uint64_t>(st.st_size) == offset,
					"Native WAL append extent or identity changed");
			writeComplete(activeLog->fd, bytes);
			syncFile(activeLog->fd);
		}
		template<class Emit>
		void installStream(const std::string &name, Emit emit) {
			// Under the exclusive directory lock, exactly one stage name is
			// reserved for this authenticated database identity. A killed writer
			// can leave this inode behind, but repeated retries cannot accumulate stages.
			const auto &tmp = stage;
			if (unlinkat(directory.fd, tmp.c_str(), 0) < 0 && errno != ENOENT)
				fail("discard previous native graph stage");
			File f(openat(directory.fd, tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
			bool renamed = false;
			try {
				emit(f.fd);
				syncFile(f.fd);
				if (::close(std::exchange(f.fd, -1)) < 0)
					fail("close native graph stage");
				if (renameat(directory.fd, tmp.c_str(), directory.fd, name.c_str()) < 0)
					fail("install native graph stage");
				renamed = true;
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
		void install(const std::string &name, Span b) {
			installStream(name, [&](int fd) { writeComplete(fd, b); });
		}
		Digest installBase(State &s) {
			utils::Sha256 hash;
			struct stat written{};
			CheckpointLocations nodes, edges;
			size_t nodeStart = 0, edgeStart = 0;
			installStream(main, [&](int fd) {
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
				head.magic("ZYXng003");
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
				Encoder encoded(maxState);
				VersionCharge rowBuffer(s.budget, 0);
				nodeStart = head.bytes.size();
				size_t offset = nodeStart;
				auto rows = [&](const auto &table, CheckpointLocations &layout) {
					table.visitAfter(0, [&](auto it) {
						auto id = it.id();
						auto size = it.encodedBytes() - 16;
						require(size <= maxState, "Native checkpoint row exceeds encoding limit");
						// A large-file API can outgrow the in-memory offset type
						// on a 32-bit host. Include this header and the final digest.
						if constexpr (sizeof(off_t) > sizeof(size_t))
							require(offset <= SIZE_MAX - size - 16 - Digest{}.size(),
									"Native checkpoint exceeds addressable extent");
						if (size > encoded.bytes.capacity()) {
							// The previous buffer is empty and can be freed before growing.
							Bytes{}.swap(encoded.bytes);
							rowBuffer.growTo(size);
							encoded.bytes.reserve(size);
						}
						if (it.onDisk()) {
							encoded.bytes.resize(size);
							it.readImage(encoded.bytes);
							layout.observe(id, it.offset(), offset + 16);
						} else {
							encodeBaseRow(encoded, it->second);
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
				flush();
				writeComplete(fd, hash.finalize());
				written = metadata(fd);
			});
			auto [published, extent] = openInput(main);
			auto st = metadata(published->fd);
			require(sameContentStamp(written, st), "Published native base identity changed");
			remember(published, st);
			nodes.bind(s.nodes, published, nodeStart, s.budget);
			edges.bind(s.edges, published, edgeStart, s.budget);
			return hash.finalize();
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
	std::string Journal::create(const std::string &name) {
		std::filesystem::path target(name);
		require(!target.filename().empty() && target.filename() != "." && target.filename() != "..",
				"Invalid native graph directory");
		File parent(open((target.has_parent_path() ? target.parent_path() : ".").c_str(),
						 O_RDONLY | O_DIRECTORY | O_CLOEXEC));
		if (mkdirat(parent.fd, target.filename().c_str(), 0700) < 0)
			fail("exclusively create native graph directory");
		syncFile(parent.fd);
		auto path = target / "main";
		Files files(path, false);
		State s;
		s.identity = identity();
		files.stage = stageName(s.identity);
		files.installBase(s);
		return path.string();
	}
	Journal::Journal(const std::string &path, bool ro, size_t memory) :
		files(std::make_unique<Files>(path, ro)), readOnly(ro) {
		auto s = std::make_shared<State>(memory);
		auto [main, extent] = files->openInput(files->main);
		require(extent >= 112, "Incomplete native graph main");
		auto initial = Files::metadata(main->fd);
		Cursor m(main->fd, extent, s->budget, memory);
		auto head = m.fixed<80>();
		Decoder r{head};
		r.magic("ZYXng003");
		std::ranges::copy(r.take(16), s->identity.begin());
		files->stage = stageName(s->identity);
		baseSequence = r.num();
		auto sequence = r.num();
		auto needed = r.count(maxWal);
		auto expected = r.take(32);
		require(baseSequence <= sequence && ((baseSequence == sequence) == (needed == 0)),
				"Invalid native graph receipt");
		s->sequence = baseSequence;
		auto finish = [](Cursor &c) {
			auto hash = c.hash.finalize();
			auto tail = c.fixed<32>();
			require(!c.remaining() && hash == tail, "Native graph main checksum mismatch");
			return hash;
		};
		auto loadBase = [&](Cursor &c, const std::shared_ptr<File> &file) {
			s->nodeHigh = c.num();
			s->edgeHigh = c.num();
			require(s->nodeHigh <= INT64_MAX && s->edgeHigh <= INT64_MAX, "Invalid native base ID high-water mark");
			auto nodes = c.count(c.remaining() / 16), edges = c.count(c.remaining() / 16);
			const bool lazy = c.remaining() > std::min(size_t(256 * 1024), memory / 2);
			auto rows = [&](size_t n, uint64_t high, auto &table, auto decode) {
				uint64_t prior = 0;
				table.loadSorted(n, [&] {
					auto id = c.num();
					auto size = c.count(maxState);
					require(id > prior && id <= high && size <= c.remaining(), "Invalid native base row extent");
					prior = id;
					VersionCharge scratch(s->budget, 3 * size + 4096);
					auto offset = c.position;
					Bytes b(size);
					c.consume(b);
					auto row = decode(id, b);
					if constexpr (std::is_same_v<decltype(row), LogicalGraphEdge>)
						require(s->nodes.contains(row.source_node_id) && s->nodes.contains(row.target_node_id),
								"Native WAL dangling relationship");
					if (!lazy)
						return table.residentRecord(id, std::move(row));
					return table.baseRecord(id, size + 16, offset, digest(b), decode);
				});
				table.bindInput(file);
			};
			rows(nodes, s->nodeHigh, s->nodes, baseNode);
			rows(edges, s->edgeHigh, s->edges, baseEdge);
			baseDigest = finish(c);
		};
		if (!needed) {
			require(std::ranges::equal(digest({}), expected), "Invalid standalone digest");
			loadBase(m, main);
			files->remember(main, initial);
			needsStabilization =
					files->exists(files->anchor()) || files->exists(files->log) || files->exists(files->stage);
		} else {
			finish(m);
			auto [wal, length] = files->openInput(files->log);
			require(length >= needed, "Missing native graph WAL prefix");
			Cursor log(wal->fd, needed, s->budget, memory);
			// A receipt fitting one admitted window can be replayed directly
			// from those authenticated bytes. Larger WALs rehash during replay.
			log.authenticateWal(expected);
			auto wh = log.fixed<72>();
			Decoder h{wh};
			h.magic("ZYXnw003");
			require(std::ranges::equal(s->identity, h.take(16)) && h.num() == baseSequence,
					"Native graph WAL generation mismatch");
			auto alignment = h.count(65536);
			require(alignment >= 4096 && (alignment & (alignment - 1)) == 0, "Invalid native WAL alignment");
			auto anchor = files->openInput(files->anchor());
			Cursor a(anchor.first->fd, anchor.second, s->budget, memory);
			auto ah = a.fixed<80>();
			Decoder ar{ah};
			ar.magic("ZYXng003");
			require(std::ranges::equal(ar.take(16), s->identity) && ar.num() == baseSequence &&
							ar.num() == baseSequence && ar.num() == 0 && std::ranges::equal(ar.take(32), digest({})),
					"Native graph base generation mismatch");
			loadBase(a, anchor.first);
			require(std::ranges::equal(h.take(32), baseDigest), "Native graph base digest mismatch");
			while (log.remaining()) {
				auto header = log.fixed<24>();
				Decoder tx{header};
				tx.magic("ZYXtx002");
				auto seq = tx.num();
				require(s->sequence < UINT64_MAX && seq == s->sequence + 1, "Native graph WAL sequence gap");
				auto size = tx.count(maxState);
				VersionCharge charge(s->budget, 3 * size + std::min(size_t(16384), 8 * size));
				Bytes delta(size);
				log.consume(delta);
				applyDelta(*s, delta);
				s->sequence = seq;
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
			require(std::ranges::equal(walHash.finalize(), expected) && s->sequence == sequence,
					"Incomplete native graph transaction");
		}
		current = std::move(s);
	}
	Journal::~Journal() = default;
	std::shared_ptr<const State> Journal::snapshot() {
		std::lock_guard lock(gate);
		require(!poisoned, "Native graph requires recovery");
		return current;
	}
	void Journal::checkpointLocked() {
		auto rebased = std::make_shared<State>(*current);
		auto fresh = files->installBase(*rebased);
		files->removeWal();
		current = std::move(rebased);
		baseDigest = fresh;
		baseSequence = current->sequence;
		walLength = 0;
		walHash = utils::Sha256{};
		needsStabilization = false;
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
	void Journal::commit(const std::shared_ptr<const State> &expected, std::shared_ptr<State> next,
						 const Changes &changed) {
		std::lock_guard lock(gate);
		require(!readOnly && !poisoned, "Native graph writer unavailable");
		// A checkpoint may replace the physical root at the same revision.
		// The budget is owned by exactly one open journal and shared by every
		// derived root; matching a copied file's identity alone is insufficient.
		if (current != expected && (!expected || current->budget != expected->budget ||
									current->sequence != expected->sequence))
			throw LogicalGraphMutationConflict("Stale native graph write snapshot");
		require(current->sequence < UINT64_MAX, "Native graph sequence exhausted");
		next->sequence = current->sequence + 1;
		auto delta = encodeDelta(*next, changed);
		try {
			if (needsStabilization || delta.size() + 24 + files->alignment - 1 > maxWal - walLength)
				checkpointLocked();
			// Unchanged entries in this private transaction still carry the
			// original offsets. Inherit the locator map published for that
			// logical version, even if those bytes moved within the new file.
			if (current != expected) {
				next->nodes.inheritInput(current->nodes);
				next->edges.inheritInput(current->edges);
			}
			Encoder bytes(maxWal);
			if (walLength == 0) {
				bytes.magic("ZYXnw003");
				bytes.raw(next->identity);
				bytes.num(baseSequence);
				bytes.num(files->alignment);
				bytes.raw(baseDigest);
			}
			bytes.magic("ZYXtx002");
			bytes.num(next->sequence);
			bytes.num(delta.size());
			bytes.raw(delta);
			// Seal the current file/VM block before acknowledging its contents.
			// The next append starts at a new boundary even after a short write.
			size_t padding = (files->alignment - bytes.bytes.size() % files->alignment) % files->alignment;
			bytes.room(padding);
			bytes.bytes.resize(bytes.bytes.size() + padding);
			auto hash = walHash;
			hash.update(bytes.bytes);
			auto length = walLength + bytes.bytes.size();
			auto receipt = manifest(*next, baseSequence, {}, length, hash.finalize());
			if (walLength == 0) {
				files->retainBase();
				files->install(files->log, bytes.bytes);
			} else
				files->append(bytes.bytes, walLength);
			files->install(files->main, receipt);
			if (!walLength)
				files->checkBaseName(true);
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
	std::string Journal::create(const std::string &) { unavailable(); }
	Journal::Journal(const std::string &, bool, size_t) { unavailable(); }
	Journal::~Journal() = default;
	std::shared_ptr<const State> Journal::snapshot() { unavailable(); }
	void Journal::commit(const std::shared_ptr<const State> &, std::shared_ptr<State>, const Changes &) {
		unavailable();
	}
	void Journal::checkpoint() { unavailable(); }
} // namespace graph::storage::nativewal
#endif
