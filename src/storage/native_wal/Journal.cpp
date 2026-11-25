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
		Bytes manifest(const State &s, uint64_t baseSequence, Span base, Span wal) {
			Encoder w(maxState + 256);
			w.magic("ZYXng001");
			w.raw(s.identity);
			w.num(baseSequence);
			w.num(s.sequence);
			w.num(wal.size());
			w.raw(digest(wal));
			w.raw(base);
			w.raw(digest(w.bytes));
			return std::move(w.bytes);
		}
	} // namespace
	struct Journal::Files {
		File directory;
		std::string main, log, stage;
		Files(const std::filesystem::path &path, bool ro) :
			directory(::open((path.has_parent_path() ? path.parent_path() : ".").c_str(),
							 O_DIRECTORY | O_RDONLY | O_CLOEXEC)),
			main(path.filename().string()), log(main + ".wal") {
			if (flock(directory.fd, (ro ? LOCK_SH : LOCK_EX) | LOCK_NB) < 0)
				fail("lock native graph directory");
		}
		Bytes read(const std::string &name, size_t cap) {
			int opened = openat(directory.fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (opened < 0 && errno == ENOENT && name == log)
				throw std::runtime_error("Native graph receipt requires missing WAL");
			File f(opened);
			struct stat st{};
			if (fstat(f.fd, &st) < 0)
				fail("stat native graph file");
			require(st.st_nlink == 1, "Native graph main and WAL require private, unaliased files");
			require(S_ISREG(st.st_mode) && st.st_size >= 0 && static_cast<uint64_t>(st.st_size) <= cap,
					"Invalid native graph file extent");
			Bytes b(st.st_size);
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
				while (!b.empty()) {
					auto n = ::write(f.fd, b.data(), b.size());
					if (n < 0 && errno == EINTR)
						continue;
					if (n < 0)
						fail("write native graph stage");
					require(n > 0, "Zero native graph write");
					b = b.subspan(n);
				}
				syncFile(f.fd);
				if (::close(std::exchange(f.fd, -1)) < 0)
					fail("close native graph stage");
				if (renameat(directory.fd, tmp.c_str(), directory.fd, name.c_str()) < 0)
					fail("install native graph stage");
				renamed = true;
				syncFile(directory.fd);
			} catch (...) {
				if (!renamed)
					unlinkat(directory.fd, tmp.c_str(), 0);
				throw;
			}
		}
		void removeWal() {
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
		files.install(files.main, manifest(s, 0, base, {}));
		return path.string();
	}
	Journal::Journal(const std::string &path, bool ro) : files(std::make_unique<Files>(path, ro)), readOnly(ro) {
		auto data = files->read(files->main, maxState + 256);
		require(data.size() >= 104, "Incomplete native graph main");
		Span main(data);
		require(std::ranges::equal(digest(main.first(main.size() - 32)), main.last(32)),
				"Native graph main checksum mismatch");
		Decoder r{main.first(main.size() - 32)};
		r.magic("ZYXng001");
		auto s = std::make_shared<State>();
		std::ranges::copy(r.take(16), s->identity.begin());
		files->stage = stageName(s->identity);
		baseSequence = r.num();
		auto sequence = r.num();
		auto needed = r.count(maxWal);
		auto expected = r.take(32);
		require(baseSequence <= sequence && ((baseSequence == sequence) == (needed == 0)),
				"Invalid native graph receipt");
		base.assign(r.bytes.begin(), r.bytes.end());
		applyDelta(*s, base);
		s->sequence = baseSequence;
		if (needed) {
			wal = files->read(files->log, maxWal);
			require(wal.size() >= needed, "Missing native graph WAL prefix");
			wal.resize(needed);
		}
		require(std::ranges::equal(digest(wal), expected), "Native graph WAL receipt mismatch");
		if (needed) {
			Decoder log{wal};
			log.magic("ZYXnw001");
			require(std::ranges::equal(s->identity, log.take(16)) && log.num() == baseSequence,
					"Native graph WAL generation mismatch");
			while (!log.bytes.empty()) {
				auto seq = log.num();
				require(s->sequence < UINT64_MAX && seq == s->sequence + 1, "Native graph WAL sequence gap");
				applyDelta(*s, log.take(log.count(maxState)));
				s->sequence = seq;
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
		auto merged = encodeBase(*current);
		auto record = manifest(*current, current->sequence, merged, {});
		// Make an independent, fresh copy durable before dropping a possibly
		// necessary log. A subsequent fsync alone cannot repair failed writeback.
		files->install(files->main, record);
		files->removeWal();
		base = std::move(merged);
		baseSequence = current->sequence;
		wal.clear();
		needsStabilization = false;
	}
	void Journal::checkpoint() {
		std::lock_guard lock(gate);
		if (readOnly)
			return;
		require(!poisoned, "Native graph requires recovery");
		try {
			if (needsStabilization || !wal.empty())
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
		(void) encodeBase(*next); // Ensure a committed state remains checkpointable.
		try {
			if (needsStabilization || delta.size() + 16 > maxWal - wal.size())
				checkpointLocked();
			Encoder log(maxWal);
			if (wal.empty()) {
				log.magic("ZYXnw001");
				log.raw(next->identity);
				log.num(baseSequence);
			} else
				log.raw(wal);
			log.num(next->sequence);
			log.num(delta.size());
			log.raw(delta);
			auto receipt = manifest(*next, baseSequence, base, log.bytes);
			files->install(files->log, log.bytes);
			files->install(files->main, receipt);
			wal = std::move(log.bytes);
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
	Journal::Journal(const std::string &, bool) { unavailable(); }
	Journal::~Journal() = default;
	std::shared_ptr<const State> Journal::snapshot() { unavailable(); }
	void Journal::commit(const std::shared_ptr<const State> &, std::shared_ptr<State>, const Changes &) {
		unavailable();
	}
	void Journal::checkpoint() { unavailable(); }
} // namespace graph::storage::nativewal
#endif
