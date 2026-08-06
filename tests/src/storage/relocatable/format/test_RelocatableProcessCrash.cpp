/**
 * @file test_RelocatableProcessCrash.cpp
 * @brief Real child-process kill and reopen coverage for A/B Superblock publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "TestProcessSupport.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace graph::storage;

namespace graph::test {

	int runRelocatableCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
								 const std::filesystem::path &markerPath);

} // namespace graph::test

namespace {

	constexpr std::string_view CRASH_CHILD_BEFORE_PUBLICATION = "before-publication";
	constexpr std::string_view CRASH_CHILD_AFTER_PUBLICATION = "after-publication";
	constexpr auto MARKER_TIMEOUT = std::chrono::seconds(10);

	Superblock superblock(uint64_t generation, uint64_t checkpointLsn, uint64_t rootSlot) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		result.catalog_root = {PhysicalSlotId(rootSlot), 1, Generation(generation)};
		result.page_directory_root = {PhysicalSlotId(rootSlot + 1U), 2, Generation(generation)};
		result.extent_directory_root = {PhysicalSlotId(rootSlot + 2U), 3, Generation(generation)};
		result.allocator_root = {PhysicalSlotId(rootSlot + 3U), 4, Generation(generation)};
		return result;
	}

	void writeMarker(const std::filesystem::path &markerPath) {
		std::ofstream marker(markerPath, std::ios::binary | std::ios::trunc);
		if (!marker.is_open()) {
			throw std::runtime_error("crash-test child cannot create publication marker");
		}
		marker << "ready\n";
		marker.flush();
		if (!marker) {
			throw std::runtime_error("crash-test child cannot flush publication marker");
		}
	}

	void writeFailureMarker(const std::filesystem::path &markerPath, std::string_view reason) noexcept {
		std::ofstream failure(markerPath.string() + ".error", std::ios::binary | std::ios::trunc);
		if (failure.is_open()) {
			failure << reason;
		}
	}

	[[nodiscard]] std::string readFailureMarker(const std::filesystem::path &markerPath) {
		std::ifstream failure(markerPath.string() + ".error", std::ios::binary);
		if (!failure.is_open()) {
			return {};
		}
		return {std::istreambuf_iterator<char>(failure), std::istreambuf_iterator<char>()};
	}

	[[nodiscard]] bool waitForMarker(const std::filesystem::path &markerPath) {
		const auto deadline = std::chrono::steady_clock::now() + MARKER_TIMEOUT;
		while (std::chrono::steady_clock::now() < deadline) {
			std::error_code error;
			if (std::filesystem::exists(markerPath, error) && !error &&
				std::filesystem::file_size(markerPath, error) > 0U && !error) {
				return true;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return false;
	}

	[[noreturn]] void waitToBeKilled() {
		for (;;) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	class CrashChild final {
	public:
		CrashChild(std::filesystem::path databasePath, std::filesystem::path markerPath, std::string_view mode) :
			databasePath_(std::move(databasePath)), markerPath_(std::move(markerPath)) {
#ifdef _WIN32
			startWindows(mode);
#else
			startPosix(mode);
#endif
		}

		CrashChild(const CrashChild &) = delete;
		CrashChild &operator=(const CrashChild &) = delete;

		~CrashChild() { terminate(); }

		[[nodiscard]] bool waitForMarker(const std::filesystem::path &markerPath) const {
			return ::waitForMarker(markerPath);
		}

		[[nodiscard]] std::string failureReason() {
			const std::string failure = readFailureMarker(markerPath_);
			if (!failure.empty()) {
				return failure;
			}
#ifndef _WIN32
			if (running_) {
				int status = 0;
				const pid_t result = waitpid(pid_, &status, WNOHANG);
				if (result == pid_) {
					running_ = false;
					pid_ = -1;
					if (WIFEXITED(status)) {
						return "crash-test child exited with status " + std::to_string(WEXITSTATUS(status));
					}
					if (WIFSIGNALED(status)) {
						return "crash-test child terminated by signal " + std::to_string(WTERMSIG(status));
					}
					return "crash-test child exited before writing its marker";
				}
			}
#endif
			return "crash-test child did not write its marker";
		}

		void terminate() {
			if (!running_) {
				return;
			}
#ifdef _WIN32
			ASSERT_NE(process_.hProcess, nullptr);
			static_cast<void>(TerminateProcess(process_.hProcess, 137));
			EXPECT_EQ(WaitForSingleObject(process_.hProcess, 5000), WAIT_OBJECT_0);
			CloseHandle(process_.hThread);
			CloseHandle(process_.hProcess);
			process_ = {};
#else
			EXPECT_EQ(kill(pid_, SIGKILL), 0);
			int status = 0;
			EXPECT_EQ(waitpid(pid_, &status, 0), pid_);
			EXPECT_TRUE(WIFSIGNALED(status));
			EXPECT_EQ(WTERMSIG(status), SIGKILL);
			pid_ = -1;
#endif
			running_ = false;
		}

	private:
#ifdef _WIN32
		[[nodiscard]] static std::wstring quoteWindowsArgument(const std::filesystem::path &argument) {
			const std::wstring value = argument.wstring();
			std::wstring result = L"\"";
			size_t backslashes = 0;
			for (const wchar_t character: value) {
				if (character == L'\\') {
					++backslashes;
					continue;
				}
				if (character == L'\"') {
					result.append(backslashes * 2U + 1U, L'\\');
					result.push_back(character);
					backslashes = 0;
					continue;
				}
				result.append(backslashes, L'\\');
				backslashes = 0;
				result.push_back(character);
			}
			result.append(backslashes * 2U, L'\\');
			result.push_back(L'\"');
			return result;
		}

		void startWindows(std::string_view mode) {
			const std::filesystem::path executable = graph::test::testExecutablePath();
			const std::filesystem::path databasePath = databasePath_;
			const std::filesystem::path markerPath = markerPath_;
			const std::wstring command = quoteWindowsArgument(executable) + L" --zyx-relocatable-crash-child " +
										 std::wstring(mode.begin(), mode.end()) + L" " +
										 quoteWindowsArgument(databasePath) + L" " + quoteWindowsArgument(markerPath);
			std::wstring mutableCommand = command;
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
								&startup, &process_)) {
				throw std::runtime_error("crash-test parent cannot launch child process");
			}
			running_ = true;
		}
#else
		void startPosix(std::string_view mode) {
			pid_ = fork();
			if (pid_ < 0) {
				throw std::runtime_error("crash-test parent cannot fork child process");
			}
			if (pid_ == 0) {
				const int status = graph::test::runRelocatableCrashChild(mode, databasePath_, markerPath_);
				_Exit(status);
			}
			running_ = true;
		}
#endif

		std::filesystem::path databasePath_;
		std::filesystem::path markerPath_;
		bool running_ = false;
#ifdef _WIN32
		PROCESS_INFORMATION process_{};
#else
		pid_t pid_ = -1;
#endif
	};

} // namespace

namespace graph::test {

	int runRelocatableCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
								 const std::filesystem::path &markerPath) {
		try {
			if (mode != CRASH_CHILD_BEFORE_PUBLICATION && mode != CRASH_CHILD_AFTER_PUBLICATION) {
				return 2;
			}
			auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary |
																			 std::ios::trunc);
			if (!file->is_open()) {
				return 3;
			}
			auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			SuperblockFile bootstrap(io);
			bootstrap.initializeNew(superblock(1, 10, 100));

			const std::array<std::byte, 128> durableData{};
			io->writeAt(RSS_FILE_BOOTSTRAP_BYTES, durableData.data(), durableData.size());
			io->sync();
			if (mode == CRASH_CHILD_AFTER_PUBLICATION) {
				bootstrap.publish(superblock(2, 11, 200));
			}
			writeMarker(markerPath);
			waitToBeKilled();
		} catch (const std::exception &error) {
			writeFailureMarker(markerPath, error.what());
			return 4;
		} catch (...) {
			writeFailureMarker(markerPath, "unknown exception");
			return 5;
		}
	}

} // namespace graph::test

class RelocatableProcessCrashTest : public ::testing::Test {
protected:
	std::filesystem::path databasePath;
	std::filesystem::path markerPath;

	void SetUp() override {
		const auto uuid = boost::uuids::random_generator()();
		databasePath =
				std::filesystem::temp_directory_path() / ("relocatable_process_crash_" + to_string(uuid) + ".db");
		markerPath = databasePath.string() + ".marker";
	}

	void TearDown() override {
		std::error_code error;
		std::filesystem::remove(databasePath, error);
		std::filesystem::remove(markerPath, error);
		std::filesystem::remove(markerPath.string() + ".error", error);
	}

	[[nodiscard]] Superblock recover() const {
		auto file = std::make_shared<std::fstream>(databasePath, std::ios::in | std::ios::out | std::ios::binary);
		EXPECT_TRUE(file->is_open());
		auto io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		return SuperblockFile(std::move(io)).recover();
	}
};

TEST_F(RelocatableProcessCrashTest, KillBeforePublicationRetainsTheLastDurableGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_BEFORE_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	EXPECT_EQ(recover().generation, Generation(1));
	EXPECT_EQ(recover().catalog_root.physical_slot, PhysicalSlotId(100));
}

TEST_F(RelocatableProcessCrashTest, KillAfterPublicationRecoversTheNewDurableGeneration) {
	CrashChild child(databasePath, markerPath, CRASH_CHILD_AFTER_PUBLICATION);
	ASSERT_TRUE(child.waitForMarker(markerPath)) << child.failureReason();
	child.terminate();
	EXPECT_EQ(recover().generation, Generation(2));
	EXPECT_EQ(recover().catalog_root.physical_slot, PhysicalSlotId(200));
}
