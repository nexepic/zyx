/**
 * @file BenchmarkFramework.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/26
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// Platform specific includes for memory monitoring
#ifdef _WIN32
#define NOMINMAX
#include <psapi.h>
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

// Include Public API
#include "metrix/metrix.hpp"

namespace metrix::benchmark {

	struct Metrics {
		double totalTimeMs;
		double opsPerSec;
		double avgLatencyUs;
		double p99LatencyUs;
		size_t peakMemoryKB;
	};

	class SystemMonitor {
	public:
		static size_t getPeakRSS() {
#ifdef _WIN32
			PROCESS_MEMORY_COUNTERS pmc;
			if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
				return pmc.PeakWorkingSetSize / 1024; // Bytes -> KB
			}
			return 0;
#else
			rusage rusage{};
			getrusage(RUSAGE_SELF, &rusage);
#ifdef __APPLE__
			return static_cast<size_t>(rusage.ru_maxrss) / 1024; // macOS returns Bytes -> KB
#else
			return (size_t) rusage.ru_maxrss; // Linux returns KB
#endif
#endif
		}
	};

	class BenchmarkBase {
	public:
		BenchmarkBase(std::string name, std::string dbPath, int iterations, int dataSize) :
			name_(std::move(name)), dbPath_(std::move(dbPath)), iterations_(iterations), dataSize_(dataSize) {}

		virtual ~BenchmarkBase() = default;

		virtual void setup(Database &db) = 0;
		virtual void run(Database &db) = 0;
		virtual void teardown(Database &) {}

		Metrics execute() {
			// 1. Cleanup & Init
			if (std::filesystem::exists(dbPath_)) {
				std::filesystem::remove_all(dbPath_);
			}

			// Scope for DB lifecycle
			{
				Database db(dbPath_);
				db.open();

				// 2. Setup (Data Preparation)
				std::cout << "   [Setup] " << name_ << "..." << std::flush;
				setup(db);
				// Flush setup data to disk to measure query perf purely (or keep in mem for write perf)
				// db.save();
				std::cout << " Done." << std::endl;

				// 3. Execution Loop
				std::vector<double> latencies;
				latencies.reserve(iterations_);

				auto startTotal = std::chrono::high_resolution_clock::now();

				for (int i = 0; i < iterations_; ++i) {
					auto t0 = std::chrono::high_resolution_clock::now();
					run(db);
					auto t1 = std::chrono::high_resolution_clock::now();
					latencies.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
				}

				auto endTotal = std::chrono::high_resolution_clock::now();

				// 4. Teardown
				teardown(db);
				db.close();

				// 5. Calculate
				double totalMs = std::chrono::duration<double, std::milli>(endTotal - startTotal).count();
				std::ranges::sort(latencies);

				return Metrics{totalMs, iterations_ / (totalMs / 1000.0), totalMs * 1000.0 / iterations_,
							   latencies[static_cast<size_t>(iterations_ * 0.99)], SystemMonitor::getPeakRSS()};
			}
		}

		std::string getName() const { return name_; }

		virtual int getItemsPerOp() const { return 1; }

	protected:
		std::string name_;
		std::string dbPath_;
		int iterations_;
		int dataSize_;
	};

	inline std::string getSeparator() {
		// Widths correspond to: " " + setw(N)
		// Name: 1+35=36, Others: 1+15=16
		return "+" + std::string(36, '-') + "+" + std::string(16, '-') + "+" + std::string(16, '-') + "+" +
			   std::string(16, '-') + "+" + std::string(16, '-') + "+";
	}

	inline void printHeader() {
		std::string sep = getSeparator();
		std::cout << "\n"
				  << sep << "\n"
				  << "| " << std::left << std::setw(35) << "Benchmark Name"
				  << "| " << std::setw(15) << "Ops/Sec"
				  << "| " << std::setw(15) << "Avg(us)"
				  << "| " << std::setw(15) << "P99(us)"
				  << "| " << std::setw(15) << "Mem(KB)"
				  << "|\n"
				  << sep << "\n";
	}

	inline void printRow(const std::string &name, const Metrics &m) {
		std::cout << "| " << std::left << std::setw(35) << name << "| " << std::setw(15) << std::fixed
				  << std::setprecision(2) << m.opsPerSec << "| " << std::setw(15) << std::fixed << std::setprecision(2)
				  << m.avgLatencyUs << "| " << std::setw(15) << std::fixed << std::setprecision(2) << m.p99LatencyUs
				  << "| " << std::setw(15) << m.peakMemoryKB << "|\n";
	}

} // namespace metrix::benchmark
