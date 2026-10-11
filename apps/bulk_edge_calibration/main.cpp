/**
 * @file main.cpp
 * @brief Reproducible native bulk-edge ingestion calibration runner.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 **/

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <psapi.h>
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "graph/debug/PerfTrace.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "api/DatabaseBulkInternal.hpp"
#include "zyx/zyx.hpp"

namespace {

	struct Options {
		std::filesystem::path data_path;
		std::filesystem::path output_path;
		std::string dataset_label;
		std::string hardware_label;
		size_t edge_count = 10'000U;
		bool keep_data = false;
		bool include_trace_phases = false;
		size_t maximum_trace_phases = 16U;
		bool enforce_gates = false;
		double minimum_ingest_edges_per_second = std::numeric_limits<double>::quiet_NaN();
			double maximum_bytes_per_edge = std::numeric_limits<double>::quiet_NaN();
		double maximum_reopen_and_scan_ms = std::numeric_limits<double>::quiet_NaN();
			uint64_t maximum_peak_rss_bytes = 0U;
			uint64_t maximum_scratch_candidate_segments = 0U;
			uint64_t minimum_scratch_source_runs = 0U;
			uint64_t minimum_scratch_merge_passes = 0U;
	};

	struct CalibrationGateResult {
		bool enforced = false;
		bool passed = true;
		std::vector<std::string> violations;
	};

		struct Metrics {
		struct Scratch {
			uint64_t candidate_segments = 0U;
			uint64_t candidate_capacity_segments = 0U;
			uint64_t candidate_limit = 0U;
			uint64_t token_candidates = 0U;
			uint64_t token_resolver_candidates = 0U;
			uint64_t outgoing_adjacency_candidates = 0U;
			uint64_t incoming_adjacency_candidates = 0U;
			uint64_t large_value_content_candidates = 0U;
			uint64_t source_run_count = 0U;
				uint64_t merge_passes = 0U;
			};

		double endpoint_setup_ms = 0.0;
		double staging_ms = 0.0;
		double commit_ms = 0.0;
		double reopen_ms = 0.0;
		double scan_ms = 0.0;
		double close_ms = 0.0;
		double reopen_and_scan_ms = 0.0;
		uint64_t file_bytes = 0U;
			uint64_t peak_rss_bytes = 0U;
			std::optional<Scratch> scratch;
		std::vector<std::string> sidecar_files;
		std::vector<std::pair<std::string, double>> commit_phases_ms;
		std::vector<std::pair<std::string, double>> scan_phases_ms;
	};

	// A calibration measures one transaction, not one unbounded append. This is
	// the native backend's default append fairness ceiling, below its 10M
	// transaction-wide admission limit.
	constexpr size_t BULK_CALIBRATION_APPEND_RECORDS = 65'536U;
	constexpr std::string_view BULK_CALIBRATION_EDGE_TYPE = "BULK_EDGE_CALIBRATION";

	class OwnedTemporaryDirectory {
	public:
		explicit OwnedTemporaryDirectory(const bool keep) : keep_(keep), path_(create()) {}

		OwnedTemporaryDirectory(const OwnedTemporaryDirectory &) = delete;
		OwnedTemporaryDirectory &operator=(const OwnedTemporaryDirectory &) = delete;

		~OwnedTemporaryDirectory() {
			if (!keep_) {
				std::error_code error;
				std::filesystem::remove_all(path_, error);
			}
		}

		[[nodiscard]] const std::filesystem::path &path() const noexcept { return path_; }

	private:
		[[nodiscard]] static std::filesystem::path create() {
			std::error_code error;
			const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
			if (error) {
				throw std::runtime_error("could not resolve the system temporary directory: " + error.message());
			}

			const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
			for (uint32_t attempt = 0U; attempt != 1'024U; ++attempt) {
				const std::filesystem::path candidate =
						temporary / ("zyx-bulk-edge-calibration-" + std::to_string(now) + "-" + std::to_string(attempt));
				error.clear();
				if (std::filesystem::create_directory(candidate, error)) {
					return candidate;
				}
				if (error) {
					throw std::runtime_error("could not create a dedicated calibration directory: " + error.message());
				}
			}
			throw std::runtime_error("could not reserve a unique calibration directory");
		}

		bool keep_;
		std::filesystem::path path_;
	};

	[[nodiscard]] double elapsedMilliseconds(const std::chrono::steady_clock::time_point start) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
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

	[[nodiscard]] std::string jsonEscape(std::string_view value) {
		std::ostringstream output;
		for (const unsigned char character: value) {
			if (character == '\\') {
				output << "\\\\";
			} else if (character == '"') {
				output << "\\\"";
			} else if (character < 0x20U) {
				output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
					   << static_cast<unsigned int>(character) << std::dec;
			} else {
				output << static_cast<char>(character);
			}
		}
		return output.str();
	}

	[[nodiscard]] std::vector<std::string> existingSidecars(const std::filesystem::path &databasePath) {
		// The native backend is single-file. Check conventional WAL/SHM suffixes
		// explicitly so a calibration cannot attest to that property accidentally.
		const std::array<std::string_view, 4U> suffixes = {"-wal", "-shm", ".wal", ".shm"};
		std::vector<std::string> sidecars;
		for (const std::string_view suffix: suffixes) {
			const std::filesystem::path candidate = databasePath.string() + std::string(suffix);
			std::error_code error;
			if (std::filesystem::exists(candidate, error)) {
				if (error) {
					throw std::runtime_error("could not inspect a bulk-edge calibration sidecar: " + error.message());
				}
				sidecars.push_back(candidate.filename().string());
			} else if (error) {
				throw std::runtime_error("could not inspect a bulk-edge calibration sidecar: " + error.message());
			}
		}
		return sidecars;
	}

	void verifyPersistedBulkEdgeCount(graph::storage::LogicalGraphReadSnapshot &snapshot,
									const size_t expectedCount) {
		// The public query engine deliberately caps aggregate inputs at one million
		// rows. This calibration must validate its documented 10M workload without
		// weakening that production guard, so it uses the same bounded, topology-only
		// storage snapshot that backs the count optimization.
		size_t count = 0U;
		snapshot.visitEdgeTopologies(expectedCount + 1U, [&count, expectedCount](
																 const graph::storage::LogicalGraphEdgeTopology &edge) {
			if (count == expectedCount) {
				throw std::runtime_error("reopened database exposed more bulk Edges than expected");
			}
			if (edge.edge_id == 0U || edge.source_node_id == 0U || edge.target_node_id == 0U ||
				edge.type != BULK_CALIBRATION_EDGE_TYPE) {
				throw std::runtime_error("reopened database exposed an invalid bulk Edge topology");
			}
			++count;
			return true;
		});
		if (count != expectedCount) {
			throw std::runtime_error("reopened database exposed " + std::to_string(count) +
								 " bulk Edges instead of " + std::to_string(expectedCount));
		}
	}


	void verifyContiguousIds(const std::vector<int64_t> &ids, const size_t expectedCount,
							 std::optional<int64_t> &nextExpectedId) {
		if (ids.size() != expectedCount || ids.empty() || ids.front() <= 0) {
			throw std::runtime_error("bulk ingestion returned an invalid Edge ID range");
		}
		if (nextExpectedId.has_value() && ids.front() != *nextExpectedId) {
			throw std::runtime_error("bulk ingestion did not preserve its contiguous Edge ID stream");
		}
		for (size_t index = 1U; index < ids.size(); ++index) {
			if (ids[index] != ids.front() + static_cast<int64_t>(index)) {
				throw std::runtime_error("bulk ingestion did not reserve one contiguous Edge ID range");
			}
		}
		if (ids.back() == std::numeric_limits<int64_t>::max()) {
			throw std::runtime_error("bulk ingestion exhausted the public Edge ID range");
		}
		nextExpectedId = ids.back() + 1;
	}

		[[nodiscard]] uint64_t requireTraceValue(const graph::debug::PerfTrace::Snapshot &snapshot,
											 std::string_view key) {
			const auto entry = snapshot.find(std::string(key));
			if (entry == snapshot.end() || entry->second.valueCalls != 1U || entry->second.totalValue < 0) {
				throw std::runtime_error("bulk-edge calibration did not capture one bounded scratch metric");
			}
			return static_cast<uint64_t>(entry->second.totalValue);
		}

		void verifyCandidateBreakdown(const Metrics::Scratch &scratch) {
			uint64_t total = 0U;
			for (const uint64_t value: {scratch.token_candidates, scratch.token_resolver_candidates,
									scratch.outgoing_adjacency_candidates, scratch.incoming_adjacency_candidates,
									scratch.large_value_content_candidates}) {
				if (value > std::numeric_limits<uint64_t>::max() - total) {
					throw std::runtime_error("bulk-edge calibration candidate breakdown overflowed");
				}
				total += value;
			}
			if (total != scratch.candidate_capacity_segments ||
				scratch.candidate_segments == 0U || scratch.candidate_segments > scratch.candidate_capacity_segments) {
				throw std::runtime_error("bulk-edge calibration candidate capacity breakdown disagrees with materialization");
			}
		}

		void captureTraceResults(std::vector<std::pair<std::string, double>> &destination, Metrics &metrics,
								 const Options &options, const bool captureScratch) {
			const graph::debug::PerfTrace::Snapshot snapshot = graph::debug::PerfTrace::snapshotAndReset();
			if (captureScratch) {
				metrics.scratch = Metrics::Scratch{
						requireTraceValue(snapshot, "native_graph.streamed.scratch.candidate_segments"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.candidate_capacity_segments"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.candidate_limit"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.token_candidates"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.token_resolver_candidates"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.outgoing_adjacency_candidates"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.incoming_adjacency_candidates"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.large_value_content_candidates"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.source_run_count"),
						requireTraceValue(snapshot, "native_graph.streamed.scratch.merge_passes")};
				verifyCandidateBreakdown(*metrics.scratch);
			}
			if (!options.include_trace_phases) {
				return;
			}
			for (const auto &[name, entry]: snapshot) {
				if (entry.calls == 0U) {
					continue;
				}
				destination.emplace_back(name, static_cast<double>(entry.totalNs) / 1'000'000.0);
		}
		std::ranges::sort(destination, std::greater<>{}, &std::pair<std::string, double>::second);
		if (destination.size() > options.maximum_trace_phases) {
			destination.resize(options.maximum_trace_phases);
		}
	}

	[[nodiscard]] Metrics run(const Options &options, const std::filesystem::path &databasePath) {
		Metrics metrics;
		std::vector<int64_t> sourceIds;
		std::vector<int64_t> targetIds;
		sourceIds.reserve(std::min(options.edge_count, BULK_CALIBRATION_APPEND_RECORDS));
		targetIds.reserve(std::min(options.edge_count, BULK_CALIBRATION_APPEND_RECORDS));

		{
			zyx::Database database(databasePath.string());
			database.open();

			std::cerr << "[1/4] Creating bulk-edge endpoints..." << std::endl;
			const auto endpointStart = std::chrono::steady_clock::now();
			const int64_t source = database.createNode("BulkEdgeSource");
			const int64_t target = database.createNode("BulkEdgeTarget");
			metrics.endpoint_setup_ms = elapsedMilliseconds(endpointStart);

			auto transaction = database.beginBulkTransaction();
			std::cerr << "[2/4] Staging " << options.edge_count
					  << " Edges in one transaction with bounded append batches..." << std::endl;
			const auto stagingStart = std::chrono::steady_clock::now();
			const std::vector<zyx::detail::BulkPropertyColumn> columns;
			std::optional<int64_t> nextExpectedId;
			for (size_t staged = 0U; staged < options.edge_count;) {
				const size_t batchSize = std::min(BULK_CALIBRATION_APPEND_RECORDS, options.edge_count - staged);
				sourceIds.assign(batchSize, source);
				targetIds.assign(batchSize, target);
				const std::vector<int64_t> ids = zyx::detail::DatabaseBulkInternal::createEdgesColumnar(
						database, transaction, "BULK_EDGE_CALIBRATION", sourceIds, targetIds, columns);
				verifyContiguousIds(ids, batchSize, nextExpectedId);
				staged += batchSize;
			}
			metrics.staging_ms = elapsedMilliseconds(stagingStart);

			std::cerr << "[3/4] Publishing the bulk transaction..." << std::endl;
			const auto commitStart = std::chrono::steady_clock::now();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setEnabled(options.include_trace_phases);
			graph::debug::PerfTrace::setValueCollectionEnabled(true);
			try {
				transaction.commit();
			} catch (...) {
				graph::debug::PerfTrace::setEnabled(false);
				graph::debug::PerfTrace::reset();
				throw;
			}
			graph::debug::PerfTrace::setEnabled(false);
			captureTraceResults(metrics.commit_phases_ms, metrics, options, true);
			metrics.commit_ms = elapsedMilliseconds(commitStart);
			database.close();
		}

		std::cerr << "[4/4] Reopening and scanning persisted adjacency..." << std::endl;
		const auto reopenStart = std::chrono::steady_clock::now();
		{
			graph::storage::RelocatableStorageBackendOptions reopenOptions;
			reopenOptions.data_path = databasePath;
			reopenOptions.open_mode = graph::storage::OpenMode::OPEN_EXISTING_FILE;
			graph::storage::RelocatableStorageBackend reopened(std::move(reopenOptions));
			const auto openStart = std::chrono::steady_clock::now();
			reopened.open();
			metrics.reopen_ms = elapsedMilliseconds(openStart);
			const auto scanStart = std::chrono::steady_clock::now();
			graph::debug::PerfTrace::reset();
			graph::debug::PerfTrace::setEnabled(options.include_trace_phases);
			std::unique_ptr<graph::storage::LogicalGraphReadSnapshot> snapshot =
					reopened.openLogicalGraphReadSnapshot();
			verifyPersistedBulkEdgeCount(*snapshot, options.edge_count);
			snapshot.reset();
			graph::debug::PerfTrace::setEnabled(false);
			captureTraceResults(metrics.scan_phases_ms, metrics, options, false);
			metrics.scan_ms = elapsedMilliseconds(scanStart);
			const auto closeStart = std::chrono::steady_clock::now();
			reopened.close();
			metrics.close_ms = elapsedMilliseconds(closeStart);
		}
		metrics.reopen_and_scan_ms = elapsedMilliseconds(reopenStart);

		std::error_code error;
		const uintmax_t bytes = std::filesystem::file_size(databasePath, error);
		if (error || bytes > std::numeric_limits<uint64_t>::max()) {
			throw std::runtime_error("could not measure the calibration database file");
		}
		metrics.file_bytes = static_cast<uint64_t>(bytes);
		metrics.sidecar_files = existingSidecars(databasePath);
		if (!metrics.sidecar_files.empty()) {
			throw std::runtime_error("bulk-edge calibration requires one database file and found a WAL/SHM sidecar");
		}
		metrics.peak_rss_bytes = peakResidentBytes();
		return metrics;
	}

	[[nodiscard]] CalibrationGateResult evaluateGate(const Options &options, const Metrics &metrics) {
		CalibrationGateResult result;
		result.enforced = options.enforce_gates;
		if (!options.enforce_gates) {
			return result;
		}
		const double ingestSeconds = (metrics.staging_ms + metrics.commit_ms) / 1'000.0;
		const double edgesPerSecond = ingestSeconds > 0.0 ? static_cast<double>(options.edge_count) / ingestSeconds : 0.0;
		const double bytesPerEdge = static_cast<double>(metrics.file_bytes) / static_cast<double>(options.edge_count);
		auto require = [&result](const bool condition, std::string name) {
			if (!condition) {
				result.passed = false;
				result.violations.push_back(std::move(name));
			}
		};
		require(edgesPerSecond >= options.minimum_ingest_edges_per_second, "ingest_edges_per_second");
		require(bytesPerEdge <= options.maximum_bytes_per_edge, "bytes_per_edge");
			require(metrics.reopen_and_scan_ms <= options.maximum_reopen_and_scan_ms, "reopen_and_scan_ms");
			require(metrics.peak_rss_bytes <= options.maximum_peak_rss_bytes, "peak_rss_bytes");
		require(metrics.scratch.has_value() &&
						metrics.scratch->candidate_segments <= options.maximum_scratch_candidate_segments,
					"scratch_candidate_segments");
		require(metrics.scratch.has_value() &&
						metrics.scratch->source_run_count >= options.minimum_scratch_source_runs,
					"scratch_source_run_count");
		require(metrics.scratch.has_value() &&
						metrics.scratch->merge_passes >= options.minimum_scratch_merge_passes,
					"scratch_merge_passes");
		return result;
	}

	void renderTracePhases(std::ostringstream &output, const std::vector<std::pair<std::string, double>> &phases) {
		output << '[';
		for (size_t index = 0U; index < phases.size(); ++index) {
			const auto &[name, milliseconds] = phases[index];
			output << (index == 0U ? "" : ", ") << "{\"name\": \"" << jsonEscape(name)
				   << "\", \"milliseconds\": " << milliseconds << '}';
		}
		output << ']';
	}

	void renderStringArray(std::ostringstream &output, const std::vector<std::string> &values) {
		output << '[';
		for (size_t index = 0U; index < values.size(); ++index) {
			output << (index == 0U ? "" : ", ") << '"' << jsonEscape(values[index]) << '"';
		}
		output << ']';
	}

	[[nodiscard]] std::string renderReport(const Options &options, const Metrics &metrics,
										const CalibrationGateResult &gate) {
		const double ingestSeconds = (metrics.staging_ms + metrics.commit_ms) / 1'000.0;
		const double edgesPerSecond = ingestSeconds > 0.0 ? static_cast<double>(options.edge_count) / ingestSeconds : 0.0;
		const double bytesPerEdge = static_cast<double>(metrics.file_bytes) / static_cast<double>(options.edge_count);
		std::ostringstream output;
		output.imbue(std::locale::classic());
		output << std::setprecision(17);
			output << "{\n"
				   << "  \"schema_version\": 3,\n"
			   << "  \"dataset\": {\"label\": \"" << jsonEscape(options.dataset_label)
			   << "\", \"generated\": true, \"edge_count\": " << options.edge_count << "},\n"
			   << "  \"hardware_label\": \"" << jsonEscape(options.hardware_label) << "\",\n"
			   << "  \"storage\": {\"single_file\": true, \"database_bytes\": " << metrics.file_bytes
			   << ", \"bytes_per_edge\": " << bytesPerEdge << ", \"sidecar_files\": ";
		renderStringArray(output, metrics.sidecar_files);
		output << "},\n"
			   << "  \"ingest\": {\"staging_ms\": " << metrics.staging_ms << ", \"commit_ms\": "
			   << metrics.commit_ms << ", \"edges_per_second\": " << edgesPerSecond << "},\n"
			   << "  \"reopen_scan\": {\"reopen_ms\": " << metrics.reopen_ms << ", \"scan_ms\": "
			   << metrics.scan_ms << ", \"close_ms\": " << metrics.close_ms << ", \"total_ms\": "
			   << metrics.reopen_and_scan_ms << "},\n"
			   << "  \"endpoint_setup_ms\": " << metrics.endpoint_setup_ms << ",\n"
			   << "  \"peak_rss_bytes\": " << metrics.peak_rss_bytes << ",\n"
			   << "  \"trace\": {\"enabled\": " << (options.include_trace_phases ? "true" : "false")
			   << ", \"maximum_phases\": " << options.maximum_trace_phases << ", \"commit\": ";
		renderTracePhases(output, metrics.commit_phases_ms);
		output << ", \"scan\": ";
		renderTracePhases(output, metrics.scan_phases_ms);
			output << "},\n"
				   << "  \"scratch\": ";
			if (metrics.scratch.has_value()) {
				output << "{\"candidate_segments\": " << metrics.scratch->candidate_segments
					   << ", \"candidate_capacity_segments\": " << metrics.scratch->candidate_capacity_segments
					   << ", \"candidate_limit\": " << metrics.scratch->candidate_limit
					   << ", \"token_candidates\": " << metrics.scratch->token_candidates
					   << ", \"token_resolver_candidates\": " << metrics.scratch->token_resolver_candidates
					   << ", \"outgoing_adjacency_candidates\": "
					   << metrics.scratch->outgoing_adjacency_candidates
					   << ", \"incoming_adjacency_candidates\": "
					   << metrics.scratch->incoming_adjacency_candidates
					   << ", \"large_value_content_candidates\": "
					   << metrics.scratch->large_value_content_candidates
					   << ", \"source_run_count\": " << metrics.scratch->source_run_count
					   << ", \"merge_passes\": " << metrics.scratch->merge_passes << '}';
			} else {
				output << "null";
			}
			output << ",\n"
				   << "  \"gate\": {\"enforced\": " << (gate.enforced ? "true" : "false")
			   << ", \"passed\": " << (gate.passed ? "true" : "false") << ", \"thresholds\": ";
		if (gate.enforced) {
			output << "{\"minimum_ingest_edges_per_second\": " << options.minimum_ingest_edges_per_second
				   << ", \"maximum_bytes_per_edge\": " << options.maximum_bytes_per_edge
					   << ", \"maximum_reopen_and_scan_ms\": " << options.maximum_reopen_and_scan_ms
					   << ", \"maximum_peak_rss_bytes\": " << options.maximum_peak_rss_bytes
					   << ", \"maximum_scratch_candidate_segments\": "
					   << options.maximum_scratch_candidate_segments
					   << ", \"minimum_scratch_source_runs\": " << options.minimum_scratch_source_runs
					   << ", \"minimum_scratch_merge_passes\": " << options.minimum_scratch_merge_passes << '}';
		} else {
			output << "null";
		}
		output << ", \"violations\": [";
		for (size_t index = 0U; index < gate.violations.size(); ++index) {
			output << (index == 0U ? "" : ", ") << '"' << jsonEscape(gate.violations[index]) << '"';
		}
		return output.str() + "]}\n}\n";
	}

	void printMetrics(const Options &options, const std::filesystem::path &databasePath, const Metrics &metrics) {
		const double ingestSeconds = (metrics.staging_ms + metrics.commit_ms) / 1'000.0;
		const double edgesPerSecond = ingestSeconds > 0.0 ? static_cast<double>(options.edge_count) / ingestSeconds : 0.0;
		std::cerr << std::fixed << std::setprecision(3);
		std::cerr << "Bulk Edge Calibration\n";
		std::cerr << "  edge_count          : " << options.edge_count << '\n';
		std::cerr << "  endpoint_setup_ms   : " << metrics.endpoint_setup_ms << '\n';
		std::cerr << "  staging_ms          : " << metrics.staging_ms << '\n';
		std::cerr << "  commit_ms           : " << metrics.commit_ms << '\n';
		std::cerr << "  reopen_ms           : " << metrics.reopen_ms << '\n';
		std::cerr << "  scan_ms             : " << metrics.scan_ms << '\n';
		std::cerr << "  close_ms            : " << metrics.close_ms << '\n';
		std::cerr << "  reopen_and_scan_ms  : " << metrics.reopen_and_scan_ms << '\n';
		std::cerr << "  ingest_edges_per_s  : " << edgesPerSecond << '\n';
		std::cerr << "  database_file_bytes : " << metrics.file_bytes << '\n';
		std::cerr << "  bytes_per_edge      : " << static_cast<double>(metrics.file_bytes) / options.edge_count << '\n';
			std::cerr << "  peak_rss_bytes      : " << metrics.peak_rss_bytes << '\n';
			if (metrics.scratch.has_value()) {
				std::cerr << "  scratch_candidates  : " << metrics.scratch->candidate_segments << '\n';
				std::cerr << "  scratch_capacity    : " << metrics.scratch->candidate_capacity_segments << '\n';
				std::cerr << "  scratch_limit       : " << metrics.scratch->candidate_limit << '\n';
				std::cerr << "  scratch_tokens      : " << metrics.scratch->token_candidates << '\n';
				std::cerr << "  scratch_token_lookup: " << metrics.scratch->token_resolver_candidates << '\n';
				std::cerr << "  scratch_outgoing    : " << metrics.scratch->outgoing_adjacency_candidates << '\n';
				std::cerr << "  scratch_incoming    : " << metrics.scratch->incoming_adjacency_candidates << '\n';
				std::cerr << "  scratch_large_value : " << metrics.scratch->large_value_content_candidates << '\n';
				std::cerr << "  scratch_source_runs : " << metrics.scratch->source_run_count << '\n';
				std::cerr << "  scratch_merge_passes: " << metrics.scratch->merge_passes << '\n';
			}
		for (const auto &[name, milliseconds]: metrics.commit_phases_ms) {
			std::cerr << "  phase_" << name << "_ms : " << milliseconds << '\n';
		}
		for (const auto &[name, milliseconds]: metrics.scan_phases_ms) {
			std::cerr << "  scan_phase_" << name << "_ms : " << milliseconds << '\n';
		}
		if (options.keep_data) {
			std::cerr << "  database_path       : " << databasePath.string() << '\n';
		}
	}

} // namespace

int main(int argc, char **argv) {
	try {
		Options options;
		CLI::App app{"Validate native bulk-edge ingest correctness and collect calibration metrics"};
		app.add_option("--data", options.data_path, "New single-file database path; must not exist");
		app.add_option("--output", options.output_path, "Write JSON report to this file instead of stdout");
		app.add_option("--dataset-label", options.dataset_label,
					   "Stable generated-workload identity; required with --enforce-gates");
		app.add_option("--hardware-label", options.hardware_label,
					   "Declared calibration hardware identity; required with --enforce-gates");
		app.add_option("--edge-count", options.edge_count,
					   "Number of Edges to create in one columnar bulk transaction (1 through 10,000,000)")
				->check(CLI::Range(static_cast<size_t>(1U), static_cast<size_t>(10'000'000U)));
		app.add_flag("--keep-data", options.keep_data,
				 "Preserve the tool-owned temporary database directory after a successful run; invalid with --data");
		app.add_flag("--include-trace-phases", options.include_trace_phases,
				 "Include a bounded PerfTrace phase summary in the report");
		app.add_option("--maximum-trace-phases", options.maximum_trace_phases)->check(CLI::PositiveNumber);
		app.add_flag("--enforce-gates", options.enforce_gates,
				 "Require all thresholds and return exit code 2 on regression");
		app.add_option("--minimum-ingest-edges-per-second", options.minimum_ingest_edges_per_second)
				->check(CLI::PositiveNumber);
		app.add_option("--maximum-bytes-per-edge", options.maximum_bytes_per_edge)->check(CLI::PositiveNumber);
			app.add_option("--maximum-reopen-and-scan-ms", options.maximum_reopen_and_scan_ms)->check(CLI::PositiveNumber);
			app.add_option("--maximum-peak-rss-bytes", options.maximum_peak_rss_bytes)->check(CLI::PositiveNumber);
		app.add_option("--maximum-scratch-candidate-segments", options.maximum_scratch_candidate_segments)
					->check(CLI::PositiveNumber);
		app.add_option("--minimum-scratch-source-runs", options.minimum_scratch_source_runs)
					->check(CLI::PositiveNumber);
		auto *minimumScratchMergePassesOption = app.add_option("--minimum-scratch-merge-passes", options.minimum_scratch_merge_passes)
					->check(CLI::Range(static_cast<uint64_t>(0U), std::numeric_limits<uint64_t>::max()));
		CLI11_PARSE(app, argc, argv);

		const bool hasAnyThreshold = std::isfinite(options.minimum_ingest_edges_per_second) ||
							 std::isfinite(options.maximum_bytes_per_edge) ||
							 std::isfinite(options.maximum_reopen_and_scan_ms) || options.maximum_peak_rss_bytes != 0U;
		const bool hasScratchThreshold = options.maximum_scratch_candidate_segments != 0U ||
								 options.minimum_scratch_source_runs != 0U ||
								 options.minimum_scratch_merge_passes != 0U;
			const bool hasEveryThreshold = std::isfinite(options.minimum_ingest_edges_per_second) &&
								   std::isfinite(options.maximum_bytes_per_edge) &&
								   std::isfinite(options.maximum_reopen_and_scan_ms) && options.maximum_peak_rss_bytes != 0U;
		const bool hasCompleteThresholds = hasEveryThreshold && options.maximum_scratch_candidate_segments != 0U &&
								   options.minimum_scratch_source_runs != 0U &&
								   minimumScratchMergePassesOption->count() == 1U;
		if (options.keep_data && !options.data_path.empty()) {
			throw std::invalid_argument("--keep-data is invalid when --data already selects the retained database path");
		}
			if (options.enforce_gates != hasCompleteThresholds || (!options.enforce_gates && (hasAnyThreshold || hasScratchThreshold))) {
				throw std::invalid_argument("bulk-edge calibration thresholds require --enforce-gates and all gate thresholds");
			}
		if (options.enforce_gates && (options.dataset_label.empty() || options.hardware_label.empty())) {
			throw std::invalid_argument("enforced bulk-edge calibration requires dataset and hardware labels");
		}
		std::optional<OwnedTemporaryDirectory> temporaryDirectory;
		if (options.data_path.empty()) {
			temporaryDirectory.emplace(options.keep_data);
			options.data_path = temporaryDirectory->path() / "bulk-edge.zyx";
		}
		if (!options.output_path.empty() &&
			std::filesystem::absolute(options.output_path).lexically_normal() ==
					std::filesystem::absolute(options.data_path).lexically_normal()) {
			throw std::invalid_argument("bulk-edge calibration report path must differ from its database path");
		}
		std::error_code error;
		if (std::filesystem::exists(options.data_path, error) || error) {
			throw std::runtime_error("bulk-edge calibration refuses to overwrite an existing database file");
		}
		const std::filesystem::path databasePath = options.data_path;
		const Metrics metrics = run(options, databasePath);
		printMetrics(options, databasePath, metrics);
		const CalibrationGateResult gate = evaluateGate(options, metrics);
		const std::string report = renderReport(options, metrics, gate);
		if (options.output_path.empty()) {
			std::cout << report;
		} else {
			std::ofstream output(options.output_path, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) {
				throw std::runtime_error("could not open bulk-edge calibration report output");
			}
			output << report;
			output.flush();
			if (!output) {
				throw std::runtime_error("could not write bulk-edge calibration report output");
			}
		}
		return gate.passed ? 0 : 2;
	} catch (const std::exception &error) {
		std::cerr << "zyx-bulk-edge-calibrate: " << error.what() << '\n';
		return 1;
	}
}
