/**
 * @file main.cpp
 * @brief Reproducible relocatable Vector query calibration runner.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
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
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <memory>
#include <set>
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

#include "graph/storage/LogicalGraphVectorSearch.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	struct CalibrationQuery {
		std::string id;
		std::vector<float> values;
	};

	struct Options {
		std::filesystem::path data_path;
		std::filesystem::path queries_path;
		std::filesystem::path output_path;
		std::string index_name;
		std::string dataset_label;
		std::string hardware_label;
		size_t top_k = 10U;
		size_t warmup_iterations = 1U;
		size_t measured_iterations = 5U;
		size_t maximum_queries = 100'000U;
		size_t maximum_mapping_entries = 10'000'000U;
		size_t maximum_raw_vector_reads = 10'000'000U;
		size_t candidate_search_budget = 1'024U;
		size_t candidate_refinement_budget = 256U;
		bool enforce_gates = false;
		double minimum_recall_at_k = std::numeric_limits<double>::quiet_NaN();
		double maximum_p50_latency_us = std::numeric_limits<double>::quiet_NaN();
		double maximum_p95_latency_us = std::numeric_limits<double>::quiet_NaN();
		double maximum_p99_latency_us = std::numeric_limits<double>::quiet_NaN();
		uint64_t maximum_peak_rss_bytes = 0U;
	};

	struct AggregateStats {
		LogicalGraphVectorSearchStats work;
		uint64_t measured_searches = 0U;
		bool all_searches_used_adjacency = true;
		bool all_searches_used_pq = true;
	};

	struct LatencyPercentiles {
		double p50 = 0.0;
		double p95 = 0.0;
		double p99 = 0.0;
	};

	struct CalibrationGateResult {
		bool enforced = false;
		bool passed = true;
		std::vector<std::string> violations;
	};

	struct DatasetFingerprint {
		uint64_t bytes = 0U;
		uint32_t crc32 = 0U;

		constexpr bool operator==(const DatasetFingerprint &) const = default;
	};

	[[nodiscard]] std::string trim(std::string value) {
		const auto notSpace = [](unsigned char value) { return value != ' ' && value != '\t' && value != '\r'; };
		const auto first = std::find_if(value.begin(), value.end(), notSpace);
		const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
		return first < last ? std::string(first, last) : std::string{};
	}

	[[nodiscard]] float parseCoordinate(std::string_view text, size_t lineNumber) {
		float result = 0.0F;
		std::istringstream input{std::string(text)};
		input.imbue(std::locale::classic());
		input >> std::noskipws >> result;
		if (!input || input.peek() != std::char_traits<char>::eof() || !std::isfinite(result)) {
			throw std::runtime_error("noncanonical or non-finite coordinate on query line " +
									 std::to_string(lineNumber));
		}
		return result;
	}

	[[nodiscard]] std::vector<float> parseCoordinates(std::string_view text, size_t lineNumber) {
		std::vector<float> result;
		size_t begin = 0U;
		while (begin <= text.size()) {
			const size_t comma = text.find(',', begin);
			const size_t end = comma == std::string_view::npos ? text.size() : comma;
			if (end == begin) {
				throw std::runtime_error("empty coordinate on query line " + std::to_string(lineNumber));
			}
			result.push_back(parseCoordinate(text.substr(begin, end - begin), lineNumber));
			if (comma == std::string_view::npos) {
				break;
			}
			begin = comma + 1U;
		}
		if (result.empty()) {
			throw std::runtime_error("empty query Vector on line " + std::to_string(lineNumber));
		}
		return result;
	}

	[[nodiscard]] std::vector<CalibrationQuery> readQueries(const Options &options) {
		std::ifstream input(options.queries_path);
		if (!input.is_open()) {
			throw std::runtime_error("could not open the calibration query file");
		}
		std::vector<CalibrationQuery> queries;
		queries.reserve(std::min<size_t>(options.maximum_queries, 4'096U));
		std::string line;
		size_t lineNumber = 0U;
		size_t dimensions = 0U;
		while (std::getline(input, line)) {
			++lineNumber;
			line = trim(std::move(line));
			if (line.empty() || line.front() == '#') {
				continue;
			}
			if (queries.size() == options.maximum_queries) {
				throw std::runtime_error("calibration query count exceeds --maximum-queries");
			}
			const size_t separator = line.find('\t');
			if (separator == std::string::npos || separator == 0U || separator + 1U == line.size() ||
				line.find('\t', separator + 1U) != std::string::npos) {
				throw std::runtime_error("query line must be: query-id<TAB>comma-separated-floats");
			}
			CalibrationQuery query{line.substr(0U, separator),
								   parseCoordinates(std::string_view(line).substr(separator + 1U), lineNumber)};
			if (query.id.find_first_of("\r\n\t") != std::string::npos) {
				throw std::runtime_error("query ID contains a control separator");
			}
			if (dimensions == 0U) {
				dimensions = query.values.size();
			} else if (query.values.size() != dimensions) {
				throw std::runtime_error("calibration queries do not share one Vector dimension");
			}
			queries.push_back(std::move(query));
		}
		if (!input.eof() || queries.empty()) {
			throw std::runtime_error(queries.empty() ? "calibration query file contains no queries"
													 : "failed while reading the calibration query file");
		}
		return queries;
	}

	[[nodiscard]] double recallAtK(const LogicalGraphVectorSearchResult &exact,
								   const LogicalGraphVectorSearchResult &approximate) {
		if (exact.hits.empty()) {
			return approximate.hits.empty() ? 1.0 : 0.0;
		}
		std::set<uint64_t> expected;
		for (const LogicalGraphVectorSearchHit &hit: exact.hits) {
			expected.insert(hit.node_id);
		}
		size_t retained = 0U;
		for (const LogicalGraphVectorSearchHit &hit: approximate.hits) {
			retained += expected.contains(hit.node_id) ? 1U : 0U;
		}
		return static_cast<double>(retained) / static_cast<double>(expected.size());
	}

	void addCounter(size_t &target, size_t value, const char *name) {
		if (value > std::numeric_limits<size_t>::max() - target) {
			throw std::overflow_error(std::string("calibration counter overflowed: ") + name);
		}
		target += value;
	}

	void addStats(AggregateStats &aggregate, const LogicalGraphVectorSearchStats &value) {
		addCounter(aggregate.work.scanned_mapping_entries, value.scanned_mapping_entries, "mapping scan");
		addCounter(aggregate.work.semantic_mapping_lookups, value.semantic_mapping_lookups, "semantic mapping");
		addCounter(aggregate.work.derived_mapping_lookups, value.derived_mapping_lookups, "derived mapping");
		addCounter(aggregate.work.candidate_discoveries, value.candidate_discoveries, "candidate discovery");
		addCounter(aggregate.work.candidate_expansions, value.candidate_expansions, "candidate expansion");
		aggregate.work.candidate_frontier_peak =
				std::max(aggregate.work.candidate_frontier_peak, value.candidate_frontier_peak);
		addCounter(aggregate.work.adjacency_record_reads, value.adjacency_record_reads, "adjacency read");
		addCounter(aggregate.work.pq_record_reads, value.pq_record_reads, "PQ read");
		addCounter(aggregate.work.raw_vector_reads, value.raw_vector_reads, "Raw read");
		aggregate.work.used_adjacency = aggregate.work.used_adjacency || value.used_adjacency;
		aggregate.work.used_pq = aggregate.work.used_pq || value.used_pq;
		aggregate.all_searches_used_adjacency = aggregate.all_searches_used_adjacency && value.used_adjacency;
		aggregate.all_searches_used_pq = aggregate.all_searches_used_pq && value.used_pq;
		if (aggregate.measured_searches == std::numeric_limits<uint64_t>::max()) {
			throw std::overflow_error("calibration measured-search counter overflowed");
		}
		++aggregate.measured_searches;
	}

	[[nodiscard]] double percentile(std::vector<double> values, double quantile) {
		if (values.empty() || quantile < 0.0 || quantile > 1.0) {
			throw std::invalid_argument("percentile requires samples and a normalized quantile");
		}
		std::ranges::sort(values);
		const double position = quantile * static_cast<double>(values.size() - 1U);
		const size_t lower = static_cast<size_t>(position);
		const size_t upper = std::min(lower + 1U, values.size() - 1U);
		const double fraction = position - static_cast<double>(lower);
		return values[lower] + (values[upper] - values[lower]) * fraction;
	}

	[[nodiscard]] LatencyPercentiles latencyPercentiles(const std::vector<double> &values) {
		return {percentile(values, 0.50), percentile(values, 0.95), percentile(values, 0.99)};
	}

	[[nodiscard]] CalibrationGateResult evaluateGate(const Options &options, double recall,
													 const LatencyPercentiles &latency, uint64_t peakRssBytes,
													 bool allSearchesUsedAdjacency, bool allSearchesUsedPq) {
		CalibrationGateResult result;
		result.enforced = options.enforce_gates;
		if (!result.enforced) {
			return result;
		}
		const auto require = [&result](bool condition, const char *metric) {
			if (!condition) {
				result.passed = false;
				result.violations.emplace_back(metric);
			}
		};
		require(recall >= options.minimum_recall_at_k, "recall_at_k");
		require(latency.p50 <= options.maximum_p50_latency_us, "latency_us.p50");
		require(latency.p95 <= options.maximum_p95_latency_us, "latency_us.p95");
		require(latency.p99 <= options.maximum_p99_latency_us, "latency_us.p99");
		require(peakRssBytes <= options.maximum_peak_rss_bytes, "peak_rss_bytes");
		require(allSearchesUsedAdjacency, "used_adjacency");
		require(allSearchesUsedPq, "used_pq");
		return result;
	}

	[[nodiscard]] uint64_t fileBytes(const std::filesystem::path &path) {
		std::error_code error;
		const uintmax_t bytes = std::filesystem::file_size(path, error);
		if (error || bytes > std::numeric_limits<uint64_t>::max()) {
			throw std::runtime_error("could not measure a calibration storage file");
		}
		return static_cast<uint64_t>(bytes);
	}

	[[nodiscard]] DatasetFingerprint fingerprintFile(const std::filesystem::path &path) {
		std::ifstream input(path, std::ios::binary);
		if (!input.is_open()) {
			throw std::runtime_error("could not open the calibration dataset for fingerprinting");
		}
		DatasetFingerprint result;
		std::array<char, 64U * 1024U> buffer{};
		while (input) {
			input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
			const std::streamsize count = input.gcount();
			if (count < 0 || static_cast<uint64_t>(count) > std::numeric_limits<uint64_t>::max() - result.bytes) {
				throw std::overflow_error("calibration dataset fingerprint size overflowed");
			}
			if (count != 0) {
				result.bytes += static_cast<uint64_t>(count);
				result.crc32 = graph::utils::updateCrc(result.crc32, buffer.data(), static_cast<size_t>(count));
			}
		}
		if (!input.eof()) {
			throw std::runtime_error("failed while fingerprinting the calibration dataset");
		}
		return result;
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
			switch (character) {
				case '\\':
					output << "\\\\";
					break;
				case '"':
					output << "\\\"";
					break;
				case '\b':
					output << "\\b";
					break;
				case '\f':
					output << "\\f";
					break;
				case '\n':
					output << "\\n";
					break;
				case '\r':
					output << "\\r";
					break;
				case '\t':
					output << "\\t";
					break;
				default:
					if (character < 0x20U) {
						output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
							   << static_cast<unsigned int>(character) << std::dec;
					} else {
						output << static_cast<char>(character);
					}
			}
		}
		return output.str();
	}

	[[nodiscard]] std::string renderReport(const Options &options, size_t dimensions, size_t queryCount,
										   double meanRecall, const AggregateStats &stats, uint64_t diskBytes,
										   uint64_t peakRssBytes, const LatencyPercentiles &latency,
										   const LatencyPercentiles &exactLatency, const DatasetFingerprint &dataset,
										   const CalibrationGateResult &gate) {
		const double searches = static_cast<double>(stats.measured_searches);
		auto average = [searches](size_t value) { return searches == 0.0 ? 0.0 : value / searches; };
		std::ostringstream output;
		output << std::fixed << std::setprecision(6);
		output << "{\n"
			   << "  \"schema_version\": 2,\n"
			   << "  \"index_name\": \"" << jsonEscape(options.index_name) << "\",\n"
			   << "  \"query_file\": \"" << jsonEscape(options.queries_path.string()) << "\",\n"
			   << "  \"dataset\": {\"label\": \"" << jsonEscape(options.dataset_label)
			   << "\", \"bytes\": " << dataset.bytes << ", \"crc32\": " << dataset.crc32 << "},\n"
			   << "  \"hardware_label\": \"" << jsonEscape(options.hardware_label) << "\",\n"
			   << "  \"query_count\": " << queryCount << ",\n"
			   << "  \"dimensions\": " << dimensions << ",\n"
			   << "  \"top_k\": " << options.top_k << ",\n"
			   << "  \"warmup_iterations\": " << options.warmup_iterations << ",\n"
			   << "  \"measured_iterations\": " << options.measured_iterations << ",\n"
			   << "  \"candidate_search_budget\": " << options.candidate_search_budget << ",\n"
			   << "  \"candidate_refinement_budget\": " << options.candidate_refinement_budget << ",\n"
			   << "  \"recall_at_k\": " << meanRecall << ",\n"
			   << "  \"latency_us\": {\"p50\": " << latency.p50 << ", \"p95\": " << latency.p95
			   << ", \"p99\": " << latency.p99 << "},\n"
			   << "  \"exact_latency_us\": {\"p50\": " << exactLatency.p50 << ", \"p95\": " << exactLatency.p95
			   << ", \"p99\": " << exactLatency.p99 << "},\n"
			   << "  \"work_per_search\": {\n"
			   << "    \"candidate_discoveries\": " << average(stats.work.candidate_discoveries) << ",\n"
			   << "    \"candidate_expansions\": " << average(stats.work.candidate_expansions) << ",\n"
			   << "    \"frontier_peak\": " << stats.work.candidate_frontier_peak << ",\n"
			   << "    \"semantic_mapping_lookups\": " << average(stats.work.semantic_mapping_lookups) << ",\n"
			   << "    \"derived_mapping_lookups\": " << average(stats.work.derived_mapping_lookups) << ",\n"
			   << "    \"adjacency_record_reads\": " << average(stats.work.adjacency_record_reads) << ",\n"
			   << "    \"pq_record_reads\": " << average(stats.work.pq_record_reads) << ",\n"
			   << "    \"raw_vector_reads\": " << average(stats.work.raw_vector_reads) << "\n"
			   << "  },\n"
			   << "  \"used_adjacency\": " << (stats.work.used_adjacency ? "true" : "false") << ",\n"
			   << "  \"used_pq\": " << (stats.work.used_pq ? "true" : "false") << ",\n"
			   << "  \"all_measured_searches_used_adjacency\": "
			   << (stats.all_searches_used_adjacency ? "true" : "false") << ",\n"
			   << "  \"all_measured_searches_used_pq\": " << (stats.all_searches_used_pq ? "true" : "false")
			   << ",\n"
			   << "  \"disk_bytes\": " << diskBytes << ",\n"
			   << "  \"peak_rss_bytes\": " << peakRssBytes << ",\n"
			   << "  \"gate\": {\n"
			   << "    \"enforced\": " << (gate.enforced ? "true" : "false") << ",\n"
			   << "    \"passed\": " << (gate.passed ? "true" : "false") << ",\n"
			   << "    \"thresholds\": ";
		if (gate.enforced) {
			output << "{\"minimum_recall_at_k\": " << options.minimum_recall_at_k
				   << ", \"maximum_p50_latency_us\": " << options.maximum_p50_latency_us
				   << ", \"maximum_p95_latency_us\": " << options.maximum_p95_latency_us
				   << ", \"maximum_p99_latency_us\": " << options.maximum_p99_latency_us
				   << ", \"maximum_peak_rss_bytes\": " << options.maximum_peak_rss_bytes << "},\n";
		} else {
			output << "null,\n";
		}
		output << "    \"violations\": [";
		for (size_t index = 0U; index < gate.violations.size(); ++index) {
			output << (index == 0U ? "" : ", ") << '"' << jsonEscape(gate.violations[index]) << '"';
		}
		output << "]\n"
			   << "  }\n"
			   << "}\n";
		return output.str();
	}

	[[nodiscard]] Options parseOptions(int argc, char **argv) {
		Options options;
		CLI::App app{"Calibrate relocatable Vector query recall, latency, and structural work"};
		app.add_option("--data", options.data_path, "Complete relocatable database file")->required();
		app.add_option("--queries", options.queries_path, "TSV: query-id<TAB>comma-separated-float-coordinates")
				->required();
		app.add_option("--output", options.output_path, "Write JSON report to this file instead of stdout");
		app.add_option("--index", options.index_name, "Vector index name")->required();
		app.add_option("--dataset-label", options.dataset_label,
					   "Stable frozen-dataset identity; required with --enforce-gates");
		app.add_option("--hardware-label", options.hardware_label,
					   "Declared calibration hardware identity; required with --enforce-gates");
		app.add_option("--top-k", options.top_k)->check(CLI::PositiveNumber);
		app.add_option("--warmup", options.warmup_iterations);
		app.add_option("--iterations", options.measured_iterations)->check(CLI::PositiveNumber);
		app.add_option("--maximum-queries", options.maximum_queries)->check(CLI::PositiveNumber);
		app.add_option("--mapping-bound", options.maximum_mapping_entries)->check(CLI::PositiveNumber);
		app.add_option("--raw-read-bound", options.maximum_raw_vector_reads)->check(CLI::PositiveNumber);
		app.add_option("--candidate-search-budget", options.candidate_search_budget)->check(CLI::PositiveNumber);
		app.add_option("--refinement-budget", options.candidate_refinement_budget)->check(CLI::PositiveNumber);
		app.add_flag("--enforce-gates", options.enforce_gates,
					 "Require every production query threshold and return exit code 2 on regression");
		app.add_option("--minimum-recall-at-k", options.minimum_recall_at_k)->check(CLI::Range(0.0, 1.0));
		app.add_option("--maximum-p50-latency-us", options.maximum_p50_latency_us)->check(CLI::PositiveNumber);
		app.add_option("--maximum-p95-latency-us", options.maximum_p95_latency_us)->check(CLI::PositiveNumber);
		app.add_option("--maximum-p99-latency-us", options.maximum_p99_latency_us)->check(CLI::PositiveNumber);
		app.add_option("--maximum-peak-rss-bytes", options.maximum_peak_rss_bytes)->check(CLI::PositiveNumber);
		try {
			app.parse(argc, argv);
		} catch (const CLI::ParseError &error) {
			std::exit(app.exit(error));
		}
		const bool hasAnyThreshold =
				std::isfinite(options.minimum_recall_at_k) || std::isfinite(options.maximum_p50_latency_us) ||
				std::isfinite(options.maximum_p95_latency_us) || std::isfinite(options.maximum_p99_latency_us) ||
				options.maximum_peak_rss_bytes != 0U;
		const bool hasEveryThreshold =
				std::isfinite(options.minimum_recall_at_k) && std::isfinite(options.maximum_p50_latency_us) &&
				std::isfinite(options.maximum_p95_latency_us) && std::isfinite(options.maximum_p99_latency_us) &&
				options.maximum_peak_rss_bytes != 0U;
		if (options.index_name.empty() || options.top_k > options.candidate_refinement_budget ||
			options.candidate_search_budget > options.maximum_mapping_entries ||
			options.candidate_refinement_budget > options.maximum_raw_vector_reads ||
			(options.enforce_gates != hasEveryThreshold) || (!options.enforce_gates && hasAnyThreshold)) {
			throw std::invalid_argument("calibration options contain incompatible names or budgets");
		}
		if (options.enforce_gates && (options.maximum_p50_latency_us > options.maximum_p95_latency_us ||
									  options.maximum_p95_latency_us > options.maximum_p99_latency_us)) {
			throw std::invalid_argument("calibration latency thresholds must be monotonic from p50 through p99");
		}
		if (options.enforce_gates && (options.dataset_label.empty() || options.hardware_label.empty())) {
			throw std::invalid_argument("enforced calibration requires dataset and hardware labels");
		}
		return options;
	}

	[[nodiscard]] int run(const Options &options) {
		const DatasetFingerprint dataset = fingerprintFile(options.queries_path);
		const std::vector<CalibrationQuery> queries = readQueries(options);
		RelocatableStorageBackendOptions backendOptions;
		backendOptions.data_path = options.data_path;
		backendOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
		backendOptions.verify_integrity_on_open = true;
		RelocatableStorageBackend backend(backendOptions);
		backend.open();
		std::unique_ptr<LogicalGraphVectorSearchSnapshot> snapshot = backend.openLogicalGraphVectorSearchSnapshot();
		if (!snapshot) {
			throw std::runtime_error("could not open a calibration Vector snapshot");
		}

		std::vector<LogicalGraphVectorSearchResult> groundTruth;
		groundTruth.reserve(queries.size());
		std::vector<double> exactMicros;
		exactMicros.reserve(queries.size());
		for (const CalibrationQuery &query: queries) {
			LogicalGraphVectorSearchRequest request{options.index_name,
													query.values,
													options.top_k,
													LogicalGraphVectorSearchMode::LGVSM_EXACT,
													options.maximum_mapping_entries,
													options.maximum_raw_vector_reads,
													options.candidate_search_budget,
													options.candidate_refinement_budget};
			const auto started = std::chrono::steady_clock::now();
			groundTruth.push_back(snapshot->searchLogicalVector(request));
			const auto stopped = std::chrono::steady_clock::now();
			exactMicros.push_back(std::chrono::duration<double, std::micro>(stopped - started).count());
		}

		LogicalGraphVectorSearchRequest approximate;
		approximate.index_name = options.index_name;
		approximate.top_k = options.top_k;
		approximate.mode = LogicalGraphVectorSearchMode::LGVSM_AUTO;
		approximate.maximum_mapping_entries = options.maximum_mapping_entries;
		approximate.maximum_raw_vector_reads = options.maximum_raw_vector_reads;
		approximate.candidate_search_budget = options.candidate_search_budget;
		approximate.candidate_refinement_budget = options.candidate_refinement_budget;
		for (size_t iteration = 0U; iteration < options.warmup_iterations; ++iteration) {
			for (const CalibrationQuery &query: queries) {
				approximate.query_vector = query.values;
				static_cast<void>(snapshot->searchLogicalVector(approximate));
			}
		}

		std::vector<double> approximateMicros;
		if (queries.size() > std::numeric_limits<size_t>::max() / options.measured_iterations) {
			throw std::overflow_error("calibration latency sample count exceeds host memory");
		}
		approximateMicros.reserve(queries.size() * options.measured_iterations);
		AggregateStats aggregate;
		double recallSum = 0.0;
		for (size_t iteration = 0U; iteration < options.measured_iterations; ++iteration) {
			for (size_t ordinal = 0U; ordinal < queries.size(); ++ordinal) {
				approximate.query_vector = queries[ordinal].values;
				const auto started = std::chrono::steady_clock::now();
				const LogicalGraphVectorSearchResult result = snapshot->searchLogicalVector(approximate);
				const auto stopped = std::chrono::steady_clock::now();
				approximateMicros.push_back(std::chrono::duration<double, std::micro>(stopped - started).count());
				recallSum += recallAtK(groundTruth[ordinal], result);
				addStats(aggregate, result.stats);
			}
		}
		const double recall = recallSum / static_cast<double>(aggregate.measured_searches);
		const uint64_t diskBytes = fileBytes(options.data_path);
		const LatencyPercentiles latency = latencyPercentiles(approximateMicros);
		const LatencyPercentiles exactLatency = latencyPercentiles(exactMicros);
		const uint64_t peakRssBytes = peakResidentBytes();
		const CalibrationGateResult gate = evaluateGate(options, recall, latency, peakRssBytes,
															 aggregate.all_searches_used_adjacency,
															 aggregate.all_searches_used_pq);
		if (fingerprintFile(options.queries_path) != dataset) {
			throw std::runtime_error("calibration query dataset changed while the run was in progress");
		}
		const std::string report =
				renderReport(options, queries.front().values.size(), queries.size(), recall, aggregate, diskBytes,
							 peakRssBytes, latency, exactLatency, dataset, gate);
		if (options.output_path.empty()) {
			std::cout << report;
		} else {
			std::ofstream output(options.output_path, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) {
				throw std::runtime_error("could not open the calibration report output");
			}
			output << report;
			output.flush();
			if (!output) {
				throw std::runtime_error("could not write the calibration report output");
			}
		}
		snapshot.reset();
		backend.close();
		return gate.passed ? 0 : 2;
	}

} // namespace

int main(int argc, char **argv) {
	try {
		return run(parseOptions(argc, argv));
	} catch (const std::exception &error) {
		std::cerr << "zyx-vector-calibrate: " << error.what() << '\n';
		return 1;
	}
}
