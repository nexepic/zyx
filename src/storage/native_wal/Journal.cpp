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
		struct File {
			int fd;
			explicit File(int f) : fd(f) {
				if (fd < 0)
					fail("open native graph file");
			}
			~File() {
				if (fd >= 0)
					::close(fd);
			}
			File(const File &) = delete;
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
		Digest digest(Span b) { return utils::Sha256::digest(b); }
		Bytes manifest(const State &s, uint64_t baseSequence, Span base, size_t length, const Digest &hash) {
			Encoder w(maxState + 256);
			w.magic("ZYXng002");
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
			struct stat info{};
			if (fstat(directory.fd, &info) < 0)
				fail("stat native graph directory");
			auto page = sysconf(_SC_PAGESIZE);
			require(page > 0 && info.st_blksize > 0, "Unknown filesystem or VM block size");
			alignment = static_cast<size_t>(std::max({long(4096), page, static_cast<long>(info.st_blksize)}));
			require(alignment <= 65536 && (alignment & (alignment - 1)) == 0, "Unsupported native WAL block alignment");
		}
		Bytes read(const std::string &name, size_t cap, size_t prefix = 0) {
			int opened = openat(directory.fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (opened < 0 && errno == ENOENT && name == log)
				throw std::runtime_error("Native graph receipt requires missing WAL");
			File f(opened);
			struct stat st{};
			if (fstat(f.fd, &st) < 0)
				fail("stat native graph file");
			require(st.st_nlink == 1, "Native graph main and WAL require private, unaliased files");
			require(S_ISREG(st.st_mode) && st.st_size >= 0 &&
							(prefix ? prefix <= cap : static_cast<uint64_t>(st.st_size) <= cap),
					"Invalid native graph file extent");
			require(static_cast<uint64_t>(st.st_size) >= prefix, "Missing native graph WAL prefix");
			Bytes b(prefix ? prefix : static_cast<size_t>(st.st_size));
			size_t pos = 0;
			while (pos < b.size()) {
				auto n = ::read(f.fd, b.data() + pos, b.size() - pos);
				if (n < 0 && errno == EINTR)
					continue;
				if (n < 0)
					fail("read native graph file");
				require(n > 0, "Short native graph file");
				pos += n;
			}
			return b;
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
			struct stat st{}, named{};
			if (fstat(activeLog->fd, &st) < 0 || fstatat(directory.fd, log.c_str(), &named, AT_SYMLINK_NOFOLLOW) < 0)
				fail("stat native append target");
			require(st.st_nlink == 1 && named.st_dev == st.st_dev && named.st_ino == st.st_ino && st.st_size >= 0 &&
							static_cast<uint64_t>(st.st_size) == offset,
					"Native WAL append extent or identity changed");
			writeComplete(activeLog->fd, bytes);
			syncFile(activeLog->fd);
		}
		void install(const std::string &name, Span b) {
			// Under the exclusive directory lock, exactly one stage name is
			// reserved for this authenticated database identity. A killed writer
			// can leave this inode behind, but repeated retries cannot accumulate stages.
			const auto &tmp = stage;
			if (unlinkat(directory.fd, tmp.c_str(), 0) < 0 && errno != ENOENT)
				fail("discard previous native graph stage");
			File f(openat(directory.fd, tmp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
			bool renamed = false;
			try {
				writeComplete(f.fd, b);
				syncFile(f.fd);
				if (::close(std::exchange(f.fd, -1)) < 0)
					fail("close native graph stage");
				if (renameat(directory.fd, tmp.c_str(), directory.fd, name.c_str()) < 0)
					fail("install native graph stage");
				renamed = true;
				syncFile(directory.fd);
				if (name == log)
					activeLog = std::make_unique<File>(
							openat(directory.fd, log.c_str(), O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC));
			} catch (...) {
				if (!renamed)
					unlinkat(directory.fd, tmp.c_str(), 0);
				throw;
			}
		}
		void removeWal() {
			activeLog.reset();
			if (unlinkat(directory.fd, log.c_str(), 0) < 0) {
				if (errno == ENOENT)
					return;
				fail("remove native graph WAL");
			}
			syncFile(directory.fd);
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
		auto base = encodeBase(s);
		files.install(files.main, manifest(s, 0, base, 0, digest({})));
		return path.string();
	}
	Journal::Journal(const std::string &path, bool ro, size_t memory) :
		files(std::make_unique<Files>(path, ro)), readOnly(ro) {
		auto data = files->read(files->main, maxState + 256);
		require(data.size() >= 104, "Incomplete native graph main");
		Span main(data);
		require(std::ranges::equal(digest(main.first(main.size() - 32)), main.last(32)),
				"Native graph main checksum mismatch");
		Decoder r{main.first(main.size() - 32)};
		r.magic("ZYXng002");
		auto s = std::make_shared<State>(memory);
		std::ranges::copy(r.take(16), s->identity.begin());
		files->stage = stageName(s->identity);
		baseSequence = r.num();
		auto sequence = r.num();
		auto needed = r.count(maxWal);
		auto expected = r.take(32);
		require(baseSequence <= sequence && ((baseSequence == sequence) == (needed == 0)),
				"Invalid native graph receipt");
		s->sequence = baseSequence;
		if (!needed) {
			require(std::ranges::equal(digest({}), expected), "Invalid standalone digest");
			base.assign(r.bytes.begin(), r.bytes.end());
			applyDelta(*s, base);
		} else {
			require(r.bytes.empty(), "Unexpected base in native receipt");
			auto wal = files->read(files->log, maxWal, needed);
			walLength = needed;
			walHash.update(wal);
			require(std::ranges::equal(walHash.finalize(), expected), "Native graph WAL receipt mismatch");
			Decoder log{wal};
			log.magic("ZYXnw002");
			require(std::ranges::equal(s->identity, log.take(16)) && log.num() == baseSequence,
					"Native graph WAL generation mismatch");
			auto alignment = log.count(65536);
			require(alignment >= 4096 && (alignment & (alignment - 1)) == 0, "Invalid native WAL alignment");
			applyDelta(*s, log.take(log.count(maxState)));
			while (!log.bytes.empty()) {
				log.magic("ZYXtx002");
				auto seq = log.num();
				require(s->sequence < UINT64_MAX && seq == s->sequence + 1, "Native graph WAL sequence gap");
				applyDelta(*s, log.take(log.count(maxState)));
				s->sequence = seq;
				size_t used = needed - log.bytes.size();
				auto padding = log.take((alignment - used % alignment) % alignment);
				require(std::ranges::all_of(padding, [](std::byte b) { return b == std::byte{}; }),
						"Invalid native WAL frame padding");
			}
		}
		require(s->sequence == sequence, "Incomplete native graph transaction");
		current = std::move(s);
	}
	Journal::~Journal() = default;
	std::shared_ptr<const State> Journal::snapshot() {
		std::lock_guard lock(gate);
		require(!poisoned, "Native graph requires recovery");
		return current;
	}
	void Journal::checkpointLocked() {
		Bytes merged;
		if (walLength)
			merged = encodeBase(*current);
		auto record = manifest(*current, current->sequence, walLength ? Span(merged) : Span(base), 0, digest({}));
		// Make an independent, fresh copy durable before dropping a possibly
		// necessary log. A subsequent fsync alone cannot repair failed writeback.
		files->install(files->main, record);
		files->removeWal();
		if (walLength)
			base = std::move(merged);
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
		if (current != expected)
			throw LogicalGraphMutationConflict("Stale native graph write snapshot");
		require(current->sequence < UINT64_MAX, "Native graph sequence exhausted");
		next->sequence = current->sequence + 1;
		auto delta = encodeDelta(*next, changed);
		require(next->encodedSize() <= maxState, "Native graph encoded state budget exceeded");
		try {
			if (needsStabilization || delta.size() + 24 + files->alignment - 1 > maxWal - walLength)
				checkpointLocked();
			Encoder bytes(maxWal);
			if (walLength == 0) {
				bytes.magic("ZYXnw002");
				bytes.raw(next->identity);
				bytes.num(baseSequence);
				bytes.num(files->alignment);
				bytes.num(base.size());
				bytes.raw(base);
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
			if (walLength == 0)
				files->install(files->log, bytes.bytes);
			else
				files->append(bytes.bytes, walLength);
			files->install(files->main, receipt);
			walLength = length;
			walHash = hash;
			Bytes{}.swap(base);
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
