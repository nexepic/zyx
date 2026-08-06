/**
 * @file main.cpp
 * @brief Reproducible relocatable Vector build and space calibration runner.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <bit>
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
#include <map>
#include <memory>
#include <optional>
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

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/allocation/PhysicalSegmentAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorPqRebuild.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorCodebookStore.hpp"
#include "graph/storage/relocatable/vector/VectorManifestStore.hpp"
#include "graph/storage/relocatable/vector/VectorPageStore.hpp"
#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"
#include "graph/vector/core/BFloat16.hpp"

using namespace graph::storage;

namespace {

	struct SourceVector {
		std::string id;
		std::vector<float> values;
		uint64_t node_id = 0U;
	};

	struct Options {
		std::filesystem::path data_path;
		std::filesystem::path vectors_path;
		std::filesystem::path output_path;
		std::string index_name = "calibration_embedding";
		std::string label = "CalibrationVector";
		std::string property = "embedding";
		std::string dataset_label;
		std::string hardware_label;
		uint64_t database_id = 0U;
		size_t maximum_vectors = 10'000'000U;
		size_t batch_size = 256U;
		size_t maximum_physical_slot = 1'000'000U;
		uint32_t pq_subspaces = 0U;
		uint32_t pq_centroids = 0U;
		uint32_t pq_iterations = 5U;
		size_t pq_maximum_samples = 100'000U;
		uint16_t adjacency_degree = 32U;
		size_t adjacency_search_budget = 128U;
		float adjacency_pruning_alpha = 1.2F;
		uint64_t codebook_id = 1'000'001U;
		uint64_t codebook_segment_id = 1'000'002U;
		uint64_t pq_mapping_suffix = 0x1001U;
		uint64_t adjacency_mapping_suffix = 0x1002U;
		bool enforce_gates = false;
		double minimum_build_vectors_per_second = std::numeric_limits<double>::quiet_NaN();
		double maximum_bytes_per_vector = std::numeric_limits<double>::quiet_NaN();
		double maximum_submitted_write_amplification = std::numeric_limits<double>::quiet_NaN();
		double maximum_file_growth_amplification = std::numeric_limits<double>::quiet_NaN();
		uint64_t maximum_peak_rss_bytes = 0U;
	};

	struct PhaseMeasurement {
		double seconds = 0.0;
		uint64_t disk_bytes_before = 0U;
		uint64_t disk_bytes_after = 0U;
		uint64_t submitted_write_bytes = 0U;
		uint64_t reserved_bytes = 0U;
	};

	struct BuildSummary {
		uint64_t final_disk_bytes = 0U;
		uint64_t submitted_write_bytes = 0U;
		uint64_t reserved_bytes = 0U;
		uint64_t file_growth_bytes = 0U;
		double total_seconds = 0.0;
		double vectors_per_second = 0.0;
		double bytes_per_vector = 0.0;
		double submitted_write_amplification = 0.0;
		double file_growth_amplification = 0.0;
		uint64_t peak_rss_bytes = 0U;
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
			throw std::runtime_error("invalid or non-finite coordinate on Vector line " + std::to_string(lineNumber));
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
				throw std::runtime_error("empty coordinate on Vector line " + std::to_string(lineNumber));
			}
			result.push_back(parseCoordinate(text.substr(begin, end - begin), lineNumber));
			if (comma == std::string_view::npos) {
				break;
			}
			begin = comma + 1U;
		}
		return result;
	}

	[[nodiscard]] std::vector<SourceVector> readVectors(const Options &options) {
		std::ifstream input(options.vectors_path);
		if (!input.is_open()) {
			throw std::runtime_error("could not open the calibration Vector file");
		}
		std::vector<SourceVector> vectors;
		vectors.reserve(std::min<size_t>(options.maximum_vectors, 65'536U));
		std::string line;
		size_t lineNumber = 0U;
		size_t dimensions = 0U;
		std::string previousId;
		while (std::getline(input, line)) {
			++lineNumber;
			line = trim(std::move(line));
			if (line.empty() || line.front() == '#') {
				continue;
			}
			if (vectors.size() == options.maximum_vectors) {
				throw std::runtime_error("Vector count exceeds --maximum-vectors");
			}
			const size_t separator = line.find('\t');
			if (separator == std::string::npos || separator == 0U || separator + 1U == line.size() ||
				line.find('\t', separator + 1U) != std::string::npos) {
				throw std::runtime_error("Vector line must be: external-id<TAB>comma-separated-floats");
			}
			SourceVector vector{line.substr(0U, separator),
								parseCoordinates(std::string_view(line).substr(separator + 1U), lineNumber), 0U};
			if (!previousId.empty() && vector.id <= previousId) {
				throw std::runtime_error("Vector external IDs must be strictly increasing for a frozen dataset");
			}
			previousId = vector.id;
			if (dimensions == 0U) {
				dimensions = vector.values.size();
			} else if (vector.values.size() != dimensions) {
				throw std::runtime_error("calibration Vectors do not share one dimension");
			}
			vectors.push_back(std::move(vector));
		}
		if (!input.eof() || vectors.empty() || dimensions == 0U || dimensions > std::numeric_limits<uint32_t>::max()) {
			throw std::runtime_error("calibration Vector file is empty, malformed, or too wide");
		}
		return vectors;
	}

	[[nodiscard]] uint64_t fileBytes(const std::filesystem::path &path) {
		std::error_code error;
		const uintmax_t result = std::filesystem::file_size(path, error);
		if (error || result > std::numeric_limits<uint64_t>::max()) {
			throw std::runtime_error("could not measure a calibration storage file");
		}
		return static_cast<uint64_t>(result);
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

	[[nodiscard]] uint64_t diskBytes(const Options &options) {
		return fileBytes(options.data_path);
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

	[[nodiscard]] uint64_t checkedLogicalBytes(size_t vectors, size_t dimensions) {
		if (dimensions == 0U || dimensions > std::numeric_limits<uint64_t>::max() / sizeof(float)) {
			throw std::overflow_error("logical Vector dataset byte count overflowed");
		}
		const uint64_t bytesPerVector = static_cast<uint64_t>(dimensions) * sizeof(float);
		if (vectors > std::numeric_limits<uint64_t>::max() / bytesPerVector) {
			throw std::overflow_error("logical Vector dataset byte count overflowed");
		}
		return static_cast<uint64_t>(vectors) * bytesPerVector;
	}

	[[nodiscard]] uint64_t growth(const PhaseMeasurement &phase) {
		if (phase.disk_bytes_after < phase.disk_bytes_before) {
			throw std::runtime_error("calibration storage unexpectedly shrank during a build phase");
		}
		return phase.disk_bytes_after - phase.disk_bytes_before;
	}

	[[nodiscard]] uint64_t checkedAdd(uint64_t left, uint64_t right, const char *name) {
		if (right > std::numeric_limits<uint64_t>::max() - left) {
			throw std::overflow_error(std::string("calibration byte counter overflowed: ") + name);
		}
		return left + right;
	}

	[[nodiscard]] size_t scanBoundForFrozenDataset(size_t vectorCount) {
		if (vectorCount == std::numeric_limits<size_t>::max()) {
			throw std::overflow_error("calibration Node scan bound overflowed");
		}
		return vectorCount + 1U;
	}

	void captureIo(PhaseMeasurement &phase, const StorageIOStats &data) {
		phase.submitted_write_bytes = data.write_bytes;
		phase.reserved_bytes = data.reserved_bytes;
	}

	void captureIo(PhaseMeasurement &phase, const RelocatableStorageIOStats &stats) {
		captureIo(phase, stats.data);
	}

	[[nodiscard]] double throughput(size_t records, double seconds) {
		return seconds <= 0.0 ? 0.0 : static_cast<double>(records) / seconds;
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
		CLI::App app{"Build a frozen relocatable Vector database and report throughput and space amplification"};
		app.add_option("--data", options.data_path, "New relocatable database file; must not exist")->required();
		app.add_option("--vectors", options.vectors_path, "TSV: external-id<TAB>comma-separated-float-coordinates")
				->required();
		app.add_option("--output", options.output_path, "Write JSON report to this file instead of stdout");
		app.add_option("--database-id", options.database_id)->required()->check(CLI::PositiveNumber);
		app.add_option("--index", options.index_name);
		app.add_option("--label", options.label);
		app.add_option("--property", options.property);
		app.add_option("--dataset-label", options.dataset_label,
					   "Stable frozen-dataset identity; required with --enforce-gates");
		app.add_option("--hardware-label", options.hardware_label,
					   "Declared calibration hardware identity; required with --enforce-gates");
		app.add_option("--maximum-vectors", options.maximum_vectors)->check(CLI::PositiveNumber);
		app.add_option("--batch-size", options.batch_size)->check(CLI::PositiveNumber);
		app.add_option("--maximum-physical-slot", options.maximum_physical_slot)->check(CLI::PositiveNumber);
		app.add_option("--pq-subspaces", options.pq_subspaces)->required()->check(CLI::PositiveNumber);
		app.add_option("--pq-centroids", options.pq_centroids)->required()->check(CLI::PositiveNumber);
		app.add_option("--pq-iterations", options.pq_iterations)->check(CLI::PositiveNumber);
		app.add_option("--pq-maximum-samples", options.pq_maximum_samples)->check(CLI::PositiveNumber);
		app.add_option("--adjacency-degree", options.adjacency_degree)->check(CLI::PositiveNumber);
		app.add_option("--adjacency-search-budget", options.adjacency_search_budget)->check(CLI::PositiveNumber);
		app.add_option("--adjacency-pruning-alpha", options.adjacency_pruning_alpha)->check(CLI::PositiveNumber);
		app.add_option("--codebook-id", options.codebook_id)->check(CLI::PositiveNumber);
		app.add_option("--codebook-segment-id", options.codebook_segment_id)->check(CLI::PositiveNumber);
		app.add_option("--pq-mapping-suffix", options.pq_mapping_suffix)->check(CLI::PositiveNumber);
		app.add_option("--adjacency-mapping-suffix", options.adjacency_mapping_suffix)->check(CLI::PositiveNumber);
		app.add_flag("--enforce-gates", options.enforce_gates,
					 "Require every production build threshold and return exit code 2 on regression");
		app.add_option("--minimum-build-vectors-per-second", options.minimum_build_vectors_per_second)
				->check(CLI::PositiveNumber);
		app.add_option("--maximum-bytes-per-vector", options.maximum_bytes_per_vector)->check(CLI::PositiveNumber);
		app.add_option("--maximum-submitted-write-amplification", options.maximum_submitted_write_amplification)
				->check(CLI::PositiveNumber);
		app.add_option("--maximum-file-growth-amplification", options.maximum_file_growth_amplification)
				->check(CLI::PositiveNumber);
		app.add_option("--maximum-peak-rss-bytes", options.maximum_peak_rss_bytes)->check(CLI::PositiveNumber);
		try {
			app.parse(argc, argv);
		} catch (const CLI::ParseError &error) {
			std::exit(app.exit(error));
		}
		const uint64_t suffixMask = ~RSS_DERIVED_MAPPING_INDEX_TYPE_MASK;
		const bool hasAnyThreshold = std::isfinite(options.minimum_build_vectors_per_second) ||
									 std::isfinite(options.maximum_bytes_per_vector) ||
									 std::isfinite(options.maximum_submitted_write_amplification) ||
									 std::isfinite(options.maximum_file_growth_amplification) ||
									 options.maximum_peak_rss_bytes != 0U;
		const bool hasEveryThreshold = std::isfinite(options.minimum_build_vectors_per_second) &&
									   std::isfinite(options.maximum_bytes_per_vector) &&
									   std::isfinite(options.maximum_submitted_write_amplification) &&
									   std::isfinite(options.maximum_file_growth_amplification) &&
									   options.maximum_peak_rss_bytes != 0U;
		const uint8_t invalid =
				static_cast<uint8_t>(options.index_name.empty() || options.label.empty() || options.property.empty()) |
				static_cast<uint8_t>(options.pq_centroids > 256U) |
				static_cast<uint8_t>(options.pq_mapping_suffix == 0U || options.pq_mapping_suffix > suffixMask) |
				static_cast<uint8_t>(options.adjacency_mapping_suffix == 0U ||
									 options.adjacency_mapping_suffix > suffixMask) |
				static_cast<uint8_t>(options.pq_mapping_suffix == options.adjacency_mapping_suffix) |
				static_cast<uint8_t>(!std::isfinite(options.adjacency_pruning_alpha) ||
									 options.adjacency_pruning_alpha < 1.0F) |
				static_cast<uint8_t>(options.maximum_physical_slot > std::numeric_limits<uint32_t>::max()) |
				static_cast<uint8_t>(options.enforce_gates != hasEveryThreshold) |
				static_cast<uint8_t>(!options.enforce_gates && hasAnyThreshold);
		if (invalid != 0U) {
			throw std::invalid_argument("Vector build calibration options contain invalid names, IDs, or bounds");
		}
		if (options.enforce_gates && (options.dataset_label.empty() || options.hardware_label.empty())) {
			throw std::invalid_argument("enforced build calibration requires dataset and hardware labels");
		}
		return options;
	}

	void initializeDatabase(const Options &options) {
		std::error_code error;
		if (std::filesystem::exists(options.data_path, error) || error) {
			throw std::runtime_error("calibration refuses to overwrite an existing database file");
		}
		auto data = std::make_shared<std::fstream>(options.data_path,
											   std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
		if (!data->is_open()) {
			throw std::runtime_error("could not create calibration database file");
		}
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		Superblock initial;
		initial.database_id = DatabaseId(options.database_id);
		initial.generation = Generation(1U);
		initial.checkpoint_lsn = LogSequenceNumber(1U);
		static_cast<void>(RelocatableMetadataStore::initializeNew(dataIo, initial));
		dataIo->sync();
		dataIo.reset();
		data->close();
	}

	[[nodiscard]] graph::PropertyValue vectorProperty(std::span<const float> values) {
		std::vector<graph::PropertyValue> result;
		result.reserve(values.size());
		for (const float value: values) {
			result.emplace_back(static_cast<double>(value));
		}
		return graph::PropertyValue(std::move(result));
	}

	[[nodiscard]] PhaseMeasurement loadGraph(const Options &options, std::vector<SourceVector> &vectors) {
		RelocatableStorageBackendOptions backendOptions;
		backendOptions.data_path = options.data_path;
		backendOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
		backendOptions.maximum_index_definitions = 16U;
		backendOptions.maximum_online_index_entries = vectors.size();
		backendOptions.maximum_online_index_scan_nodes = scanBoundForFrozenDataset(vectors.size());
		backendOptions.maximum_online_index_pages = vectors.size();
		RelocatableStorageBackend backend(backendOptions);
		backend.open();
		LogicalGraphWriteBackend &writer = backend;
		PhaseMeasurement result;
		result.disk_bytes_before = diskBytes(options);
		backend.resetIoStats();
		const auto started = std::chrono::steady_clock::now();
		for (size_t begin = 0U; begin < vectors.size(); begin += options.batch_size) {
			const size_t end = std::min(begin + options.batch_size, vectors.size());
			std::vector<LogicalGraphNodeCreateRequest> requests;
			requests.reserve(end - begin);
			for (size_t ordinal = begin; ordinal < end; ++ordinal) {
				requests.push_back({{options.label}, {{options.property, vectorProperty(vectors[ordinal].values)}}});
			}
			const std::vector<uint64_t> nodeIds = writer.createLogicalNodes(requests);
			if (nodeIds.size() != requests.size()) {
				throw std::runtime_error("calibration graph load returned an incomplete Node ID batch");
			}
			for (size_t ordinal = 0U; ordinal < nodeIds.size(); ++ordinal) {
				vectors[begin + ordinal].node_id = nodeIds[ordinal];
			}
		}
		const auto stopped = std::chrono::steady_clock::now();
		backend.flush();
		captureIo(result, backend.ioStats());
		backend.close();
		result.seconds = std::chrono::duration<double>(stopped - started).count();
		result.disk_bytes_after = diskBytes(options);
		return result;
	}

	[[nodiscard]] std::pair<PhaseMeasurement, LogicalGraphIndexDefinition>
	buildRawIndex(const Options &options, size_t vectorCount, uint32_t dimensions) {
		RelocatableStorageBackendOptions backendOptions;
		backendOptions.data_path = options.data_path;
		backendOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
		backendOptions.maximum_index_definitions = 16U;
		backendOptions.maximum_online_index_entries = vectorCount;
		backendOptions.maximum_online_index_scan_nodes = scanBoundForFrozenDataset(vectorCount);
		backendOptions.maximum_online_index_pages = vectorCount;
		RelocatableStorageBackend backend(backendOptions);
		backend.open();
		LogicalGraphSchemaBackend &schema = backend;
		PhaseMeasurement result;
		result.disk_bytes_before = diskBytes(options);
		backend.resetIoStats();
		const auto started = std::chrono::steady_clock::now();
		const LogicalGraphIndexDefinition definition = schema.createLogicalNodeVectorIndex(
				{options.index_name, options.label, options.property, dimensions, "l2"});
		const auto stopped = std::chrono::steady_clock::now();
		backend.flush();
		captureIo(result, backend.ioStats());
		backend.close();
		result.seconds = std::chrono::duration<double>(stopped - started).count();
		result.disk_bytes_after = diskBytes(options);
		return {result, definition};
	}

	[[nodiscard]] std::vector<CanonicalVectorRecord> canonicalRecords(const MetadataSnapshotPin &snapshot,
																	  uint64_t indexId, BTreePageStore &trees,
																	  const std::vector<SourceVector> &source,
																	  RelocatableMetadataStore &metadata) {
		const std::vector<BTreePageEntry> mappings =
				BTreeIndex(indexId, trees, metadata).scan(snapshot, std::nullopt, source.size() + 1U);
		if (mappings.size() != source.size()) {
			throw std::runtime_error("calibration Node-to-Vector mapping does not cover the frozen dataset");
		}
		std::map<uint64_t, const SourceVector *> byNode;
		for (const SourceVector &vector: source) {
			if (vector.node_id == 0U || !byNode.emplace(vector.node_id, &vector).second) {
				throw std::runtime_error("calibration source contains invalid or duplicate Node IDs");
			}
		}
		std::vector<CanonicalVectorRecord> result;
		result.reserve(source.size());
		for (const BTreePageEntry &mapping: mappings) {
			if (mapping.key.size() != sizeof(uint64_t) || mapping.value_or_child == 0U) {
				throw std::runtime_error("calibration encountered a malformed semantic Vector mapping");
			}
			uint64_t nodeId = 0U;
			for (const std::byte value: mapping.key) {
				nodeId = (nodeId << 8U) | std::to_integer<uint8_t>(value);
			}
			const auto sourceVector = byNode.find(nodeId);
			if (sourceVector == byNode.end() || mapping.key != VectorSemanticCodec::nodeMappingKey(nodeId)) {
				throw std::runtime_error("calibration mapping names a Node outside the frozen dataset");
			}
			result.push_back(
					{mapping.value_or_child,
					 VectorSemanticCodec::encodeBFloat16(vectorProperty(sourceVector->second->values),
														 static_cast<uint32_t>(sourceVector->second->values.size()))});
		}
		std::ranges::sort(result, {}, &CanonicalVectorRecord::vector_id);
		if (std::ranges::adjacent_find(result, {}, &CanonicalVectorRecord::vector_id) != result.end()) {
			throw std::runtime_error("calibration mapping contains duplicate Vector IDs");
		}
		return result;
	}

	struct DerivedBuildResult {
		PhaseMeasurement pq;
		PhaseMeasurement adjacency;
		VectorAdjacencyBuildWork adjacency_work;
	};

	[[nodiscard]] DerivedBuildResult buildDerived(const Options &options, const LogicalGraphIndexDefinition &definition,
									  const std::vector<SourceVector> &source) {
		auto data = std::make_shared<std::fstream>(options.data_path, std::ios::in | std::ios::out | std::ios::binary);
		if (!data->is_open()) {
			throw std::runtime_error("could not reopen calibration database file for derived rebuild");
		}
		auto dataIo = std::make_shared<StorageIO>(data, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		auto metadata = RelocatableMetadataStore::open(dataIo);
		SlottedPagePool slotted(dataIo, *metadata);
		BTreePageStore trees(slotted, *metadata);
		VectorPageStore vectorPages(dataIo, *metadata);
		VectorAdjacencyPageStore adjacencyPages(dataIo, *metadata);
		VectorManifestStore manifests(dataIo, *metadata);
		PhysicalSegmentAllocator segments(dataIo, *metadata);
		VectorCodebookStore codebooks(segments, *metadata);
		MetadataSnapshotPin snapshot = metadata->openSnapshot();
		const std::vector<CanonicalVectorRecord> records =
				canonicalRecords(snapshot, definition.index_id, trees, source, *metadata);
		snapshot.release();

		std::vector<DeterministicPqTrainingRecord> training;
		training.reserve(records.size());
		for (const CanonicalVectorRecord &record: records) {
			training.push_back({record.vector_id, record.raw_bfloat16});
		}
		const PhysicalSlotId firstSlot(1U);
		const PhysicalSlotId lastSlot(static_cast<uint32_t>(options.maximum_physical_slot));
		const uint64_t pqMappingId = RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX | options.pq_mapping_suffix;
		OfflineVectorPqRebuildOptions pqOptions;
		pqOptions.training = {definition.vector_dimensions,
							  options.pq_subspaces,
							  options.pq_centroids,
							  options.pq_iterations,
							  records.size(),
							  std::min(options.pq_maximum_samples, records.size())};
		pqOptions.codebook.codebook_id = options.codebook_id;
		pqOptions.codebook.manifest_segment_id = SegmentId(options.codebook_segment_id);
		pqOptions.codebook.generation = Generation(1U);
		pqOptions.codebook.element_type = VectorCodebookElementType::VCET_FLOAT32;
		pqOptions.codebook.dimensions = definition.vector_dimensions;
		pqOptions.codebook.subspace_count = options.pq_subspaces;
		pqOptions.codebook.centroid_count = options.pq_centroids;
		const uint64_t codebookPayloadBytes =
				static_cast<uint64_t>(definition.vector_dimensions) * options.pq_centroids * sizeof(float);
		if (codebookPayloadBytes > std::numeric_limits<size_t>::max()) {
			throw std::overflow_error("calibration Codebook payload exceeds host-addressable memory");
		}
		const size_t codebookDataExtents =
				VectorCodebookStore::requiredDataExtentCount(static_cast<size_t>(codebookPayloadBytes));
		pqOptions.codebook.data_extents.reserve(codebookDataExtents);
		for (size_t ordinal = 0U; ordinal < codebookDataExtents; ++ordinal) {
			const uint64_t delta = static_cast<uint64_t>(ordinal) + 1U;
			if (delta > std::numeric_limits<uint64_t>::max() - options.codebook_id ||
				delta > std::numeric_limits<uint64_t>::max() - options.codebook_segment_id) {
				throw std::overflow_error("calibration Codebook extent identity range overflowed");
			}
			pqOptions.codebook.data_extents.push_back(
					{ExtentId(options.codebook_id + delta), SegmentId(options.codebook_segment_id + delta)});
		}
		pqOptions.codebook_first_slot = firstSlot;
		pqOptions.codebook_last_slot = lastSlot;
		pqOptions.pages = {firstSlot, lastSlot, Generation(1U), records.size(), records.size()};
		pqOptions.mapping_index_id = pqMappingId;
		pqOptions.mapping.tree = {firstSlot, lastSlot, pqMappingId, records.size(), records.size()};
		pqOptions.mapping.maximum_entries = records.size();

		DerivedBuildResult result;
		result.pq.disk_bytes_before = diskBytes(options);
		dataIo->resetStats();
		auto started = std::chrono::steady_clock::now();
		static_cast<void>(OfflineVectorPqRebuild::run(trees, vectorPages, codebooks, manifests, *metadata,
													  definition.index_id, training, pqOptions));
		auto stopped = std::chrono::steady_clock::now();
		dataIo->sync();
		captureIo(result.pq, dataIo->stats());
		result.pq.seconds = std::chrono::duration<double>(stopped - started).count();
		result.pq.disk_bytes_after = diskBytes(options);

		const uint64_t adjacencyMappingId =
				RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX | options.adjacency_mapping_suffix;
		OfflineVectorAdjacencyRebuildOptions adjacencyOptions;
		adjacencyOptions.maximum_exact_records = records.size();
		adjacencyOptions.build_mode = VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
		adjacencyOptions.maximum_candidate_records = records.size();
		adjacencyOptions.candidate_search_budget = std::min(options.adjacency_search_budget, records.size());
		adjacencyOptions.pruning_alpha = options.adjacency_pruning_alpha;
		adjacencyOptions.maximum_degree = options.adjacency_degree;
		adjacencyOptions.adjacency_generation = Generation(1U);
		adjacencyOptions.pages = {firstSlot, lastSlot, records.size(), records.size()};
		adjacencyOptions.mapping_index_id = adjacencyMappingId;
		adjacencyOptions.mapping.tree = {firstSlot, lastSlot, adjacencyMappingId, records.size(), records.size()};
		adjacencyOptions.mapping.maximum_entries = records.size();
		result.adjacency.disk_bytes_before = result.pq.disk_bytes_after;
		dataIo->resetStats();
		started = std::chrono::steady_clock::now();
		const OfflineVectorAdjacencyRebuildResult adjacency =
				OfflineVectorAdjacencyRebuild::run(trees, vectorPages, adjacencyPages, manifests, *metadata,
												   definition.index_id, records, adjacencyOptions);
		stopped = std::chrono::steady_clock::now();
		dataIo->sync();
		captureIo(result.adjacency, dataIo->stats());
		result.adjacency.seconds = std::chrono::duration<double>(stopped - started).count();
		result.adjacency.disk_bytes_after = diskBytes(options);
		result.adjacency_work = adjacency.build_work;
		metadata.reset();
		dataIo.reset();
		data->close();
		return result;
	}

	void verifyDatabase(const Options &options) {
		RelocatableStorageBackendOptions backendOptions;
		backendOptions.data_path = options.data_path;
		backendOptions.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
		backendOptions.verify_integrity_on_open = true;
		RelocatableStorageBackend backend(backendOptions);
		backend.open();
		backend.close();
	}

	[[nodiscard]] BuildSummary summarizeBuild(size_t vectorCount, uint64_t logicalBytes, const PhaseMeasurement &load,
											  const PhaseMeasurement &raw, const DerivedBuildResult &derived,
											  uint64_t peakRssBytes) {
		BuildSummary result;
		result.final_disk_bytes = derived.adjacency.disk_bytes_after;
		result.submitted_write_bytes = checkedAdd(
				checkedAdd(load.submitted_write_bytes, raw.submitted_write_bytes, "phase writes"),
				checkedAdd(derived.pq.submitted_write_bytes, derived.adjacency.submitted_write_bytes, "derived writes"),
				"total writes");
		result.reserved_bytes = checkedAdd(
				checkedAdd(load.reserved_bytes, raw.reserved_bytes, "phase reservations"),
				checkedAdd(derived.pq.reserved_bytes, derived.adjacency.reserved_bytes, "derived reservations"),
				"total reservations");
		if (result.final_disk_bytes < load.disk_bytes_before || vectorCount == 0U || logicalBytes == 0U) {
			throw std::runtime_error("calibration build summary has an invalid storage or logical-input baseline");
		}
		result.file_growth_bytes = result.final_disk_bytes - load.disk_bytes_before;
		result.total_seconds = load.seconds + raw.seconds + derived.pq.seconds + derived.adjacency.seconds;
		result.vectors_per_second = throughput(vectorCount, result.total_seconds);
		result.bytes_per_vector = static_cast<double>(result.final_disk_bytes) / static_cast<double>(vectorCount);
		result.submitted_write_amplification =
				static_cast<double>(result.submitted_write_bytes) / static_cast<double>(logicalBytes);
		result.file_growth_amplification =
				static_cast<double>(result.file_growth_bytes) / static_cast<double>(logicalBytes);
		result.peak_rss_bytes = peakRssBytes;
		return result;
	}

	[[nodiscard]] CalibrationGateResult evaluateGate(const Options &options, const BuildSummary &summary) {
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
		require(summary.vectors_per_second >= options.minimum_build_vectors_per_second, "build_vectors_per_second");
		require(summary.bytes_per_vector <= options.maximum_bytes_per_vector, "bytes_per_vector");
		require(summary.submitted_write_amplification <= options.maximum_submitted_write_amplification,
				"submitted_write_amplification");
		require(summary.file_growth_amplification <= options.maximum_file_growth_amplification,
				"file_growth_amplification");
		require(summary.peak_rss_bytes <= options.maximum_peak_rss_bytes, "peak_rss_bytes");
		return result;
	}

	[[nodiscard]] std::string renderReport(const Options &options, size_t vectorCount, size_t dimensions,
										   uint64_t logicalBytes, const PhaseMeasurement &load,
										   const PhaseMeasurement &raw, const DerivedBuildResult &derived,
										   const BuildSummary &summary, const DatasetFingerprint &dataset,
										   const CalibrationGateResult &gate) {
		std::ostringstream output;
		output << std::fixed << std::setprecision(6);
		output << "{\n"
			   << "  \"schema_version\": 2,\n"
			   << "  \"vector_file\": \"" << jsonEscape(options.vectors_path.string()) << "\",\n"
			   << "  \"index_name\": \"" << jsonEscape(options.index_name) << "\",\n"
			   << "  \"dataset\": {\"label\": \"" << jsonEscape(options.dataset_label)
			   << "\", \"bytes\": " << dataset.bytes << ", \"crc32\": " << dataset.crc32 << "},\n"
			   << "  \"hardware_label\": \"" << jsonEscape(options.hardware_label) << "\",\n"
			   << "  \"vector_count\": " << vectorCount << ",\n"
			   << "  \"dimensions\": " << dimensions << ",\n"
			   << "  \"logical_input_bytes\": " << logicalBytes << ",\n"
			   << "  \"phases\": {\n"
			   << "    \"graph_load\": {\"seconds\": " << load.seconds
			   << ", \"vectors_per_second\": " << throughput(vectorCount, load.seconds)
			   << ", \"disk_growth_bytes\": " << growth(load)
			   << ", \"submitted_write_bytes\": " << load.submitted_write_bytes << "},\n"
			   << "    \"raw_index\": {\"seconds\": " << raw.seconds
			   << ", \"vectors_per_second\": " << throughput(vectorCount, raw.seconds)
			   << ", \"disk_growth_bytes\": " << growth(raw)
			   << ", \"submitted_write_bytes\": " << raw.submitted_write_bytes << "},\n"
			   << "    \"pq_rebuild\": {\"seconds\": " << derived.pq.seconds
			   << ", \"vectors_per_second\": " << throughput(vectorCount, derived.pq.seconds)
			   << ", \"disk_growth_bytes\": " << growth(derived.pq)
			   << ", \"submitted_write_bytes\": " << derived.pq.submitted_write_bytes << "},\n"
			   << "    \"adjacency_rebuild\": {\"seconds\": " << derived.adjacency.seconds
			   << ", \"vectors_per_second\": " << throughput(vectorCount, derived.adjacency.seconds)
			   << ", \"disk_growth_bytes\": " << growth(derived.adjacency)
			   << ", \"submitted_write_bytes\": " << derived.adjacency.submitted_write_bytes << "}\n"
			   << "  },\n"
			   << "  \"adjacency_work\": {\n"
			   << "    \"pair_distance_evaluations\": " << derived.adjacency_work.exact_pair_distance_evaluations
			   << ",\n"
			   << "    \"frontier_pushes\": " << derived.adjacency_work.candidate_frontier_pushes << ",\n"
			   << "    \"frontier_pops\": " << derived.adjacency_work.candidate_frontier_pops << ",\n"
			   << "    \"frontier_peak\": " << derived.adjacency_work.candidate_frontier_peak << ",\n"
			   << "    \"prune_pair_checks\": " << derived.adjacency_work.candidate_prune_pair_checks << ",\n"
			   << "    \"reverse_link_updates\": " << derived.adjacency_work.candidate_reverse_link_updates << "\n"
			   << "  },\n"
			   << "  \"total_build_seconds\": " << summary.total_seconds << ",\n"
			   << "  \"build_vectors_per_second\": " << summary.vectors_per_second << ",\n"
			   << "  \"final_disk_bytes\": " << summary.final_disk_bytes << ",\n"
			   << "  \"bytes_per_vector\": " << summary.bytes_per_vector << ",\n"
			   << "  \"submitted_write_bytes\": " << summary.submitted_write_bytes << ",\n"
			   << "  \"reserved_bytes\": " << summary.reserved_bytes << ",\n"
			   << "  \"submitted_write_amplification\": " << summary.submitted_write_amplification << ",\n"
			   << "  \"file_growth_amplification\": " << summary.file_growth_amplification << ",\n"
			   << "  \"peak_rss_bytes\": " << summary.peak_rss_bytes << ",\n"
			   << "  \"gate\": {\n"
			   << "    \"enforced\": " << (gate.enforced ? "true" : "false") << ",\n"
			   << "    \"passed\": " << (gate.passed ? "true" : "false") << ",\n"
			   << "    \"thresholds\": ";
		if (gate.enforced) {
			output << "{\"minimum_build_vectors_per_second\": " << options.minimum_build_vectors_per_second
				   << ", \"maximum_bytes_per_vector\": " << options.maximum_bytes_per_vector
				   << ", \"maximum_submitted_write_amplification\": " << options.maximum_submitted_write_amplification
				   << ", \"maximum_file_growth_amplification\": " << options.maximum_file_growth_amplification
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

	[[nodiscard]] int run(const Options &options) {
		const DatasetFingerprint dataset = fingerprintFile(options.vectors_path);
		std::vector<SourceVector> vectors = readVectors(options);
		const uint32_t dimensions = static_cast<uint32_t>(vectors.front().values.size());
		if (options.pq_subspaces > dimensions || dimensions % options.pq_subspaces != 0U ||
			options.adjacency_degree >= vectors.size() || options.adjacency_search_budget > vectors.size()) {
			throw std::invalid_argument("PQ shape or adjacency bounds are incompatible with the frozen dataset");
		}
		const uint64_t logicalBytes = checkedLogicalBytes(vectors.size(), dimensions);
		initializeDatabase(options);
		const PhaseMeasurement load = loadGraph(options, vectors);
		const auto [raw, definition] = buildRawIndex(options, vectors.size(), dimensions);
		const DerivedBuildResult derived = buildDerived(options, definition, vectors);
		verifyDatabase(options);
		const BuildSummary summary =
				summarizeBuild(vectors.size(), logicalBytes, load, raw, derived, peakResidentBytes());
		const CalibrationGateResult gate = evaluateGate(options, summary);
		if (fingerprintFile(options.vectors_path) != dataset) {
			throw std::runtime_error("calibration Vector dataset changed while the run was in progress");
		}
		const std::string report = renderReport(options, vectors.size(), dimensions, logicalBytes, load, raw, derived,
												summary, dataset, gate);
		if (options.output_path.empty()) {
			std::cout << report;
		} else {
			std::ofstream output(options.output_path, std::ios::binary | std::ios::trunc);
			if (!output.is_open()) {
				throw std::runtime_error("could not open the build calibration report output");
			}
			output << report;
			output.flush();
			if (!output) {
				throw std::runtime_error("could not write the build calibration report output");
			}
		}
		return gate.passed ? 0 : 2;
	}

} // namespace

int main(int argc, char **argv) {
	try {
		return run(parseOptions(argc, argv));
	} catch (const std::exception &error) {
		std::cerr << "zyx-vector-build-calibrate: " << error.what() << '\n';
		return 1;
	}
}
