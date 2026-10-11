/**
 * @file RelocatableProcessCrashSupport.hpp
 * @brief Cross-platform parent/child harness for native storage crash tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include "TestProcessSupport.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__LLVM_INSTR_PROFILE_GENERATE)
#include <profile/instr_prof_interface.h>
#endif
#endif

namespace graph::test {

	int runRelocatableCrashChild(std::string_view mode, const std::filesystem::path &databasePath,
								 const std::filesystem::path &markerPath);

	namespace relocatable_crash {

		inline constexpr auto MARKER_TIMEOUT = std::chrono::seconds(10);

		inline void writeMarker(const std::filesystem::path &markerPath) {
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

		inline void writeFailureMarker(const std::filesystem::path &markerPath, std::string_view reason) noexcept {
			std::ofstream failure(markerPath.string() + ".error", std::ios::binary | std::ios::trunc);
			if (failure.is_open()) {
				failure << reason;
			}
		}

		[[nodiscard]] inline std::string readFailureMarker(const std::filesystem::path &markerPath) {
			std::ifstream failure(markerPath.string() + ".error", std::ios::binary);
			if (!failure.is_open()) {
				return {};
			}
			return {std::istreambuf_iterator<char>(failure), std::istreambuf_iterator<char>()};
		}

		[[nodiscard]] inline bool waitForMarker(const std::filesystem::path &markerPath) {
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

		[[noreturn]] inline void waitToBeKilled() {
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
				return relocatable_crash::waitForMarker(markerPath);
			}

			[[nodiscard]] bool waitForSuccessfulExit() {
				if (!running_)
					return false;
#ifdef _WIN32
				if (WaitForSingleObject(process_.hProcess, 5000) != WAIT_OBJECT_0)
					return false;
				DWORD exitCode = 0U;
				const BOOL gotExitCode = GetExitCodeProcess(process_.hProcess, &exitCode);
				CloseHandle(process_.hThread);
				CloseHandle(process_.hProcess);
				process_ = {};
				running_ = false;
				return gotExitCode != FALSE && exitCode == 0U;
#else
				int status = 0;
				pid_t waited = -1;
				do {
					waited = waitpid(pid_, &status, 0);
				} while (waited == -1 && errno == EINTR);
				if (waited != pid_)
					return false;
				pid_ = -1;
				running_ = false;
				return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
			}

			[[nodiscard]] std::string failureReason() {
				const std::string failure = readFailureMarker(markerPath_);
				if (!failure.empty()) return failure;
#ifndef _WIN32
				if (running_) {
					int status = 0;
					if (waitpid(pid_, &status, WNOHANG) == pid_) {
						running_ = false;
						pid_ = -1;
						if (WIFEXITED(status)) return "crash-test child exited with status " + std::to_string(WEXITSTATUS(status));
						if (WIFSIGNALED(status)) return "crash-test child terminated by signal " + std::to_string(WTERMSIG(status));
						return "crash-test child exited before writing its marker";
					}
				}
#endif
				return "crash-test child did not write its marker";
			}

			void terminate() {
				if (!running_) return;
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
			[[nodiscard]] static std::wstring quoteWindowsArgument(const std::filesystem::path &argument);
			void startWindows(std::string_view mode);
#else
			void startPosix(std::string_view mode) {
				pid_ = fork();
				if (pid_ < 0) throw std::runtime_error("crash-test parent cannot fork child process");
				if (pid_ == 0) {
#if defined(__LLVM_INSTR_PROFILE_GENERATE)
					// The crash child intentionally dies through SIGKILL. After fork it
					// shares the parent's LLVM profile runtime and filename, so it must
					// never truncate the parent's coverage profile on termination.
					__llvm_profile_set_filename("/dev/null");
#endif
					_Exit(runRelocatableCrashChild(mode, databasePath_, markerPath_));
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

#ifdef _WIN32
		inline std::wstring CrashChild::quoteWindowsArgument(const std::filesystem::path &argument) {
			const std::wstring value = argument.wstring();
			std::wstring result = L"\"";
			size_t backslashes = 0U;
			for (const wchar_t character: value) {
				if (character == L'\\') { ++backslashes; continue; }
				if (character == L'\"') { result.append(backslashes * 2U + 1U, L'\\'); result.push_back(character); backslashes = 0U; continue; }
				result.append(backslashes, L'\\'); backslashes = 0U; result.push_back(character);
			}
			result.append(backslashes * 2U, L'\\'); result.push_back(L'\"');
			return result;
		}

		inline void CrashChild::startWindows(std::string_view mode) {
			const std::filesystem::path executable = testExecutablePath();
			const std::wstring command = quoteWindowsArgument(executable) + L" --zyx-relocatable-crash-child " +
					std::wstring(mode.begin(), mode.end()) + L" " + quoteWindowsArgument(databasePath_) + L" " + quoteWindowsArgument(markerPath_);
			std::wstring mutableCommand = command;
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
								&startup, &process_)) {
				throw std::runtime_error("crash-test parent cannot launch child process");
			}
			running_ = true;
		}
#endif

	} // namespace relocatable_crash

} // namespace graph::test
