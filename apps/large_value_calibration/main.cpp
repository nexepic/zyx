/**
 * @file main.cpp
 * @brief Reproducible single-file LargeValue performance and space calibration runner.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <CLI/CLI.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"

using namespace graph::storage;

namespace {

	struct Options {
		std::filesystem::path data_path;
		std::filesystem::path output_path;
		std::string dataset_label;
		std::string hardware_label;
		std::string media_type = "application/octet-stream";
		uint64_t database_id = 0U;
		size_t unique_values = 100U;
		size_t value_bytes = 64U * 1024U;
		size_t duplicate_passes = 1U;
		size_t range_offset = 0U;
		size_t range_bytes = 4096U;
		size_t range_iterations = 1U;
		size_t maximum_physical_slot = 100'000U;
		size_t maximum_dedup_candidates = RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES;
		LargeValueCodec codec = LargeValueCodec::LVC_RAW;
		LargeValueExtentClass extent_class = LargeValueExtentClass::LVEC_32K;
		bool enforce_gates = false;
		double minimum_unique_writes_per_second = std::numeric_limits<double>::quiet_NaN();
		double minimum_dedup_hit_rate = std::numeric_limits<double>::quiet_NaN();
		double maximum_range_p95_latency_us = std::numeric_limits<double>::quiet_NaN();
		double maximum_space_amplification = std::numeric_limits<double>::quiet_NaN();
		uint64_t maximum_peak_rss_bytes = 0U;
	};

	struct CalibrationGateResult {
		bool enforced = false;
		bool passed = true;
		std::vector<std::string> violations;
	};

	struct UniqueWriteMetrics {
		double seconds = 0.0;
		uint64_t logical_bytes = 0U;
		uint64_t submitted_write_bytes = 0U;
		uint64_t reserved_bytes = 0U;
		uint64_t file_bytes_before = 0U;
		uint64_t file_bytes_after = 0U;
	};

	struct DedupMetrics {
		double seconds = 0.0;
		size_t attempts = 0U;
		size_t hits = 0U;
		size_t misses = 0U;
		size_t candidates_scanned = 0U;
	};

	struct RangeReadMetrics {
		double seconds = 0.0;
		size_t operations = 0U;
		uint64_t bytes = 0U;
		uint64_t submitted_read_bytes = 0U;
		double p50_us = 0.0;
		double p95_us = 0.0;
		double p99_us = 0.0;
	};

	[[nodiscard]] uint64_t checkedMultiply(size_t left, size_t right, const char *description) {
		if (left != 0U && right > std::numeric_limits<uint64_t>::max() / left) {
			throw std::overflow_error(std::string("LargeValue calibration overflowed: ") + description);
		}
		return static_cast<uint64_t>(left) * static_cast<uint64_t>(right);
	}

	[[nodiscard]] size_t checkedMultiplySize(size_t left, size_t right, const char *description) {
		if (left != 0U && right > std::numeric_limits<size_t>::max() / left) {
			throw std::overflow_error(std::string("LargeValue calibration overflowed: ") + description);
		}
		return left * right;
	}

	[[nodiscard]] uint64_t fileBytes(const std::filesystem::path &path) {
		std::error_code error;
		const uintmax_t bytes = std::filesystem::file_size(path, error);
		if (error || bytes > std::numeric_limits<uint64_t>::max()) {
			throw std::runtime_error("could not measure the LargeValue calibration database file");
		}
		return static_cast<uint64_t>(bytes);
	}

	[[nodiscard]] uint64_t peakResidentBytes() {
#if defined(_WIN32)
		PROCESS_MEMORY_COUNTERS counters{};
		if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
			throw std::runtime_error("could not query process memory usage");
		}
		return static_cast<uint64_t>(counters.PeakWorkingSetSize);
#else
		rusage usage{};
		if (getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
			throw std::runtime_error("could not query process memory usage");
		}
#if defined(__APPLE__)
		return static_cast<uint64_t>(usage.ru_maxrss);
#else
		return static_cast<uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#endif
	}

	[[nodiscard]] std::vector<std::byte> payloadFor(size_t ordinal, size_t bytes) {
		std::vector<std::byte> payload(bytes);
		uint64_t state = (static_cast<uint64_t>(ordinal) + 1U) * 0x9E3779B97F4A7C15ULL;
		for (std::byte &value: payload) {
			state ^= state >> 12U;
			state ^= state << 25U;
			state ^= state >> 27U;
			value = static_cast<std::byte>((state * 0x2545F4914F6CDD1DULL) >> 56U);
		}
		return payload;
	}

	[[nodiscard]] LargeValueCodec parseCodec(std::string_view value) {
		if (value == "raw") {
			return LargeValueCodec::LVC_RAW;
		}
		if (value == "lz4") {
			return LargeValueCodec::LVC_LZ4;
		}
		if (value == "zstd") {
			return LargeValueCodec::LVC_ZSTD;
		}
		throw std::invalid_argument("LargeValue calibration codec must be raw, lz4, or zstd");
	}

	[[nodiscard]] LargeValueExtentClass parseExtentKiB(uint32_t value) {
		switch (value) {
			case 16U:
				return LargeValueExtentClass::LVEC_16K;
			case 32U:
				return LargeValueExtentClass::LVEC_32K;
			case 64U:
				return LargeValueExtentClass::LVEC_64K;
			case 128U:
				return LargeValueExtentClass::LVEC_128K;
			default:
				throw std::invalid_argument("LargeValue calibration extent must be one of 16, 32, 64, or 128 KiB");
		}
	}

	[[nodiscard]] double percentile(std::vector<double> samples, double quantile) {
		if (samples.empty() || quantile < 0.0 || quantile > 1.0) {
			throw std::invalid_argument("LargeValue calibration percentile requires nonempty samples and a valid quantile");
		}
		std::ranges::sort(samples);
		const size_t index = static_cast<size_t>(std::ceil(quantile * static_cast<double>(samples.size()))) - 1U;
		return samples[std::min(index, samples.size() - 1U)];
	}

	[[nodiscard]] std::string jsonEscape(std::string_view value) {
		std::ostringstream output;
		for (const unsigned char character: value) {
			if (character == '\\') {
				output << "\\\\";
			} else if (character == '"') {
				output << "\\\"";
			} else if (character < 0x20U) {
				output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned int>(character)
					   << std::dec;
			} else {
				output << static_cast<char>(character);
			}
		}
		return output.str();
	}

	[[nodiscard]] Options parseOptions(int argc, char **argv) {
		Options options;
		std::string codec = "raw";
		uint32_t extentKiB = 32U;
		CLI::App app{"Calibrate single-file LargeValue write, deduplication, range-read, and space behavior"};
		app.add_option("--data", options.data_path, "New relocatable database file; must not exist")->required();
		app.add_option("--output", options.output_path, "Write JSON report to this file instead of stdout");
		app.add_option("--database-id", options.database_id)->required()->check(CLI::PositiveNumber);
		app.add_option("--dataset-label", options.dataset_label,
					   "Stable generated-workload identity; required with --enforce-gates");
		app.add_option("--hardware-label", options.hardware_label,
					   "Declared calibration hardware identity; required with --enforce-gates");
		app.add_option("--media-type", options.media_type);
		app.add_option("--unique-values", options.unique_values)->check(CLI::PositiveNumber);
		app.add_option("--value-bytes", options.value_bytes)->check(CLI::PositiveNumber);
		app.add_option("--duplicate-passes", options.duplicate_passes)->check(CLI::PositiveNumber);
		app.add_option("--range-offset", options.range_offset);
		app.add_option("--range-bytes", options.range_bytes)->check(CLI::PositiveNumber);
		app.add_option("--range-iterations", options.range_iterations)->check(CLI::PositiveNumber);
		app.add_option("--maximum-physical-slot", options.maximum_physical_slot)->check(CLI::PositiveNumber);
		app.add_option("--maximum-dedup-candidates", options.maximum_dedup_candidates)->check(CLI::PositiveNumber);
		app.add_option("--codec", codec, "raw, lz4, or zstd");
		app.add_option("--extent-kib", extentKiB, "16, 32, 64, or 128");
		app.add_flag("--enforce-gates", options.enforce_gates,
					 "Require every production threshold and return exit code 2 on regression");
		app.add_option("--minimum-unique-writes-per-second", options.minimum_unique_writes_per_second)
				->check(CLI::PositiveNumber);
		app.add_option("--minimum-dedup-hit-rate", options.minimum_dedup_hit_rate)->check(CLI::Range(0.0, 1.0));
		app.add_option("--maximum-range-p95-latency-us", options.maximum_range_p95_latency_us)
				->check(CLI::PositiveNumber);
		app.add_option("--maximum-space-amplification", options.maximum_space_amplification)
				->check(CLI::PositiveNumber);
		app.add_option("--maximum-peak-rss-bytes", options.maximum_peak_rss_bytes)->check(CLI::PositiveNumber);
		try {
			app.parse(argc, argv);
		} catch (const CLI::ParseError &error) {
			std::exit(app.exit(error));
		}
		options.codec = parseCodec(codec);
		options.extent_class = parseExtentKiB(extentKiB);
		static_cast<void>(checkedMultiply(options.unique_values, options.value_bytes, "logical bytes"));
		static_cast<void>(checkedMultiply(options.unique_values, options.duplicate_passes, "dedup attempts"));
		static_cast<void>(checkedMultiply(
				checkedMultiplySize(options.unique_values, options.range_iterations, "range operations"), options.range_bytes,
				"range bytes"));
		const bool hasAnyThreshold = std::isfinite(options.minimum_unique_writes_per_second) ||
								 std::isfinite(options.minimum_dedup_hit_rate) ||
								 std::isfinite(options.maximum_range_p95_latency_us) ||
								 std::isfinite(options.maximum_space_amplification) || options.maximum_peak_rss_bytes != 0U;
		const bool hasEveryThreshold = std::isfinite(options.minimum_unique_writes_per_second) &&
								   std::isfinite(options.minimum_dedup_hit_rate) &&
								   std::isfinite(options.maximum_range_p95_latency_us) &&
								   std::isfinite(options.maximum_space_amplification) && options.maximum_peak_rss_bytes != 0U;
		const bool invalid = options.media_type.empty() || options.maximum_physical_slot < 4U ||
							 options.maximum_physical_slot > std::numeric_limits<uint32_t>::max() ||
							 options.range_offset > options.value_bytes ||
							 options.range_bytes > options.value_bytes - options.range_offset ||
							 options.enforce_gates != hasEveryThreshold || (!options.enforce_gates && hasAnyThreshold);
		if (invalid) {
			throw std::invalid_argument("LargeValue calibration options contain incompatible names, bounds, or gates");
		}
		if (options.enforce_gates && (options.dataset_label.empty() || options.hardware_label.empty())) {
			throw std::invalid_argument("enforced LargeValue calibration requires dataset and hardware labels");
		}
		return options;
	}

	[[nodiscard]] LargeValueWriteOptions writeOptions(const Options &options) {
		const uint32_t maximumSlot = static_cast<uint32_t>(options.maximum_physical_slot);
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(1U);
		result.last_data_slot = PhysicalSlotId(maximumSlot);
		// One publication reserves distinct slots across every pool kind. Giving
		// data and manifest pools the same candidate range preserves that safety
		// property without creating an artificial sparse file between the pools.
		result.first_manifest_slot = PhysicalSlotId(1U);
		result.last_manifest_slot = PhysicalSlotId(maximumSlot);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = options.media_type;
		result.compression.codec = options.codec;
		result.extent_class = options.extent_class;
		result.maximum_dedup_candidates = options.maximum_dedup_candidates;
		return result;
	}

	[[nodiscard]] CalibrationGateResult evaluateGate(const Options &options, const UniqueWriteMetrics &unique,
															 const DedupMetrics &dedup, const RangeReadMetrics &range,
															 uint64_t finalFileBytes, uint64_t peakRssBytes) {
		CalibrationGateResult result;
		result.enforced = options.enforce_gates;
		if (!options.enforce_gates) {
			return result;
		}
		const double writesPerSecond = unique.seconds <= 0.0 ? 0.0 : static_cast<double>(options.unique_values) / unique.seconds;
		const double dedupHitRate = dedup.attempts == 0U ? 0.0 : static_cast<double>(dedup.hits) / dedup.attempts;
		const double spaceAmplification = static_cast<double>(finalFileBytes) / unique.logical_bytes;
		auto require = [&result](bool condition, std::string name) {
			if (!condition) {
				result.passed = false;
				result.violations.push_back(std::move(name));
			}
		};
		require(writesPerSecond >= options.minimum_unique_writes_per_second, "unique_writes_per_second");
		require(dedupHitRate >= options.minimum_dedup_hit_rate, "dedup_hit_rate");
		require(range.p95_us <= options.maximum_range_p95_latency_us, "range_latency_us.p95");
		require(spaceAmplification <= options.maximum_space_amplification, "space_amplification");
		require(peakRssBytes <= options.maximum_peak_rss_bytes, "peak_rss_bytes");
		return result;
	}

	[[nodiscard]] std::string renderReport(const Options &options, const UniqueWriteMetrics &unique,
															 const DedupMetrics &dedup, const RangeReadMetrics &range,
															 uint64_t finalFileBytes, uint64_t peakRssBytes,
															 const CalibrationGateResult &gate) {
		const double writesPerSecond = unique.seconds <= 0.0 ? 0.0 : static_cast<double>(options.unique_values) / unique.seconds;
		const double dedupHitRate = dedup.attempts == 0U ? 0.0 : static_cast<double>(dedup.hits) / dedup.attempts;
		const double rangeBytesPerSecond = range.seconds <= 0.0 ? 0.0 : static_cast<double>(range.bytes) / range.seconds;
		const double spaceAmplification = static_cast<double>(finalFileBytes) / unique.logical_bytes;
		const double submittedWriteAmplification =
				static_cast<double>(unique.submitted_write_bytes) / unique.logical_bytes;
		std::ostringstream output;
		output.imbue(std::locale::classic());
		output << std::setprecision(17);
		output << "{\n"
			   << "  \"schema_version\": 1,\n"
			   << "  \"dataset\": {\"label\": \"" << jsonEscape(options.dataset_label)
			   << "\", \"generated\": true, \"unique_values\": " << options.unique_values
			   << ", \"value_bytes\": " << options.value_bytes << ", \"logical_bytes\": " << unique.logical_bytes
			   << "},\n"
			   << "  \"hardware_label\": \"" << jsonEscape(options.hardware_label) << "\",\n"
			   << "  \"storage\": {\"single_file\": true, \"database_bytes\": " << finalFileBytes
			   << ", \"file_growth_bytes\": " << (finalFileBytes - unique.file_bytes_before)
			   << ", \"space_amplification\": " << spaceAmplification << "},\n"
			   << "  \"unique_write\": {\"seconds\": " << unique.seconds
			   << ", \"values_per_second\": " << writesPerSecond
			   << ", \"submitted_write_bytes\": " << unique.submitted_write_bytes
			   << ", \"reserved_bytes\": " << unique.reserved_bytes
			   << ", \"submitted_write_amplification\": " << submittedWriteAmplification << "},\n"
			   << "  \"dedup\": {\"attempts\": " << dedup.attempts << ", \"hits\": " << dedup.hits
			   << ", \"misses\": " << dedup.misses << ", \"hit_rate\": " << dedupHitRate
			   << ", \"candidates_scanned\": " << dedup.candidates_scanned << ", \"seconds\": " << dedup.seconds
			   << "},\n"
			   << "  \"range_read\": {\"operations\": " << range.operations << ", \"bytes\": " << range.bytes
			   << ", \"seconds\": " << range.seconds << ", \"bytes_per_second\": " << rangeBytesPerSecond
			   << ", \"submitted_read_bytes\": " << range.submitted_read_bytes << ", \"latency_us\": {\"p50\": "
			   << range.p50_us << ", \"p95\": " << range.p95_us << ", \"p99\": " << range.p99_us << "}},\n"
			   << "  \"peak_rss_bytes\": " << peakRssBytes << ",\n"
			   << "  \"gate\": {\"enforced\": " << (gate.enforced ? "true" : "false")
			   << ", \"passed\": " << (gate.passed ? "true" : "false") << ", \"violations\": [";
		for (size_t index = 0U; index < gate.violations.size(); ++index) {
			output << (index == 0U ? "" : ", ") << '"' << jsonEscape(gate.violations[index]) << '"';
		}
		output << "]}\n}\n";
		return output.str();
	}

	[[nodiscard]] int run(const Options &options) {
		std::error_code error;
		if (std::filesystem::exists(options.data_path, error) || error) {
			throw std::runtime_error("LargeValue calibration refuses to overwrite an existing database file");
		}
		const LargeValueWriteOptions valueOptions = writeOptions(options);
		std::vector<LargeValueRef> references;
		references.reserve(options.unique_values);
		UniqueWriteMetrics unique;
		{
			auto data = std::make_shared<std::fstream>(options.data_path,
												   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
			if (!data->is_open()) {
				throw std::runtime_error("could not create the LargeValue calibration database file");
			}
			auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			Superblock initial;
			initial.database_id = DatabaseId(options.database_id);
			initial.generation = Generation(1U);
			initial.checkpoint_lsn = LogSequenceNumber(1U);
			auto metadata = RelocatableMetadataStore::initializeNew(io, initial);
			io->sync();
			unique.file_bytes_before = fileBytes(options.data_path);
			io->resetStats();
			LargeValueStore store(io, *metadata);
			uint64_t checkpoint = 2U;
			const auto started = std::chrono::steady_clock::now();
			for (size_t ordinal = 0U; ordinal < options.unique_values; ++ordinal) {
				if (checkpoint == std::numeric_limits<uint64_t>::max()) {
					throw std::overflow_error("LargeValue calibration checkpoint LSN space is exhausted");
				}
				const std::vector<std::byte> payload = payloadFor(ordinal, options.value_bytes);
				const LargeValueWriteResult written = store.write(payload, valueOptions, LogSequenceNumber(checkpoint++));
				if (written.content_deduplicated || written.dedup_candidates_scanned != 0U) {
					throw std::runtime_error("unique LargeValue calibration input unexpectedly deduplicated");
				}
				references.push_back(written.reference);
			}
			const auto stopped = std::chrono::steady_clock::now();
			io->sync();
			unique.seconds = std::chrono::duration<double>(stopped - started).count();
			unique.logical_bytes = checkedMultiply(options.unique_values, options.value_bytes, "logical bytes");
			unique.submitted_write_bytes = io->stats().write_bytes;
			unique.reserved_bytes = io->stats().reserved_bytes;
			unique.file_bytes_after = fileBytes(options.data_path);
			metadata.reset();
			io.reset();
			data->close();
		}

		DedupMetrics dedup;
		RangeReadMetrics range;
		{
			auto data = std::make_shared<std::fstream>(options.data_path, std::ios::in | std::ios::out | std::ios::binary);
			if (!data->is_open()) {
				throw std::runtime_error("could not reopen the LargeValue calibration database file");
			}
			auto io = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
			auto metadata = RelocatableMetadataStore::open(io);
			LargeValueStore store(io, *metadata);
			MetadataSnapshotPin checkpointSnapshot = metadata->openSnapshot();
			const uint64_t currentCheckpoint = checkpointSnapshot.superblock().checkpoint_lsn.value();
			checkpointSnapshot.release();
			if (currentCheckpoint == std::numeric_limits<uint64_t>::max()) {
				throw std::overflow_error("LargeValue calibration checkpoint LSN space is exhausted");
			}
			uint64_t checkpoint = currentCheckpoint + 1U;
			const auto dedupStarted = std::chrono::steady_clock::now();
			for (size_t pass = 0U; pass < options.duplicate_passes; ++pass) {
				for (size_t ordinal = 0U; ordinal < options.unique_values; ++ordinal) {
					if (checkpoint == std::numeric_limits<uint64_t>::max()) {
						throw std::overflow_error("LargeValue calibration checkpoint LSN space is exhausted");
					}
					const std::vector<std::byte> payload = payloadFor(ordinal, options.value_bytes);
					const LargeValueWriteResult result = store.write(payload, valueOptions, LogSequenceNumber(checkpoint++));
					++dedup.attempts;
					if (result.dedup_candidates_scanned > std::numeric_limits<size_t>::max() - dedup.candidates_scanned) {
						throw std::overflow_error("LargeValue calibration dedup candidate count overflowed");
					}
					dedup.candidates_scanned += result.dedup_candidates_scanned;
					if (result.content_deduplicated && result.reference == references[ordinal]) {
						++dedup.hits;
					} else {
						++dedup.misses;
					}
				}
			}
			const auto dedupStopped = std::chrono::steady_clock::now();
			dedup.seconds = std::chrono::duration<double>(dedupStopped - dedupStarted).count();
			if (dedup.hits != dedup.attempts || dedup.misses != 0U) {
				throw std::runtime_error("LargeValue calibration duplicate input was not persistently deduplicated");
			}

			MetadataSnapshotPin snapshot = metadata->openSnapshot();
			io->resetStats();
			std::vector<double> samples;
			samples.reserve(checkedMultiplySize(options.unique_values, options.range_iterations, "range operations"));
			const auto rangeStarted = std::chrono::steady_clock::now();
			for (size_t iteration = 0U; iteration < options.range_iterations; ++iteration) {
				for (size_t ordinal = 0U; ordinal < references.size(); ++ordinal) {
					const auto started = std::chrono::steady_clock::now();
					const std::vector<std::byte> actual =
							store.readRange(snapshot, references[ordinal], options.range_offset, options.range_bytes);
					const auto stopped = std::chrono::steady_clock::now();
					const std::vector<std::byte> expected = payloadFor(ordinal, options.value_bytes);
					if (!std::equal(actual.begin(), actual.end(),
								expected.begin() + static_cast<std::ptrdiff_t>(options.range_offset))) {
						throw std::runtime_error("LargeValue calibration range read returned incorrect bytes");
					}
					samples.push_back(std::chrono::duration<double, std::micro>(stopped - started).count());
					++range.operations;
					if (options.range_bytes > std::numeric_limits<uint64_t>::max() - range.bytes) {
						throw std::overflow_error("LargeValue calibration range byte count overflowed");
					}
					range.bytes += options.range_bytes;
				}
			}
			const auto rangeStopped = std::chrono::steady_clock::now();
			range.seconds = std::chrono::duration<double>(rangeStopped - rangeStarted).count();
			range.submitted_read_bytes = io->stats().read_bytes;
			range.p50_us = percentile(samples, 0.50);
			range.p95_us = percentile(samples, 0.95);
			range.p99_us = percentile(samples, 0.99);
			snapshot.release();
			io->sync();
			metadata.reset();
			io.reset();
			data->close();
		}

		const uint64_t finalFileBytes = fileBytes(options.data_path);
		const uint64_t peakRssBytes = peakResidentBytes();
		const CalibrationGateResult gate = evaluateGate(options, unique, dedup, range, finalFileBytes, peakRssBytes);
		const std::string report = renderReport(options, unique, dedup, range, finalFileBytes, peakRssBytes, gate);
		if (options.output_path.empty()) {
			std::cout << report;
		} else {
			std::ofstream output(options.output_path, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) {
				throw std::runtime_error("could not open LargeValue calibration report output");
			}
			output << report;
			output.flush();
			if (!output) {
				throw std::runtime_error("could not write LargeValue calibration report output");
			}
		}
		return gate.passed ? 0 : 2;
	}

} // namespace

int main(int argc, char **argv) {
	try {
		return run(parseOptions(argc, argv));
	} catch (const std::exception &error) {
		std::cerr << "zyx-large-value-calibrate: " << error.what() << '\n';
		return 1;
	}
}
