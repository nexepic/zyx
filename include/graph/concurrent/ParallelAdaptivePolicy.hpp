/**
 * @file ParallelAdaptivePolicy.hpp
 * @brief Lightweight runtime feedback for parallel worker-count decisions.
 *
 * The adaptive state intentionally depends only on the C++ standard library:
 * no OS-specific counters, no startup microbenchmarks, and no persistence.
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "graph/concurrent/ParallelExecutionTypes.hpp"

namespace graph::concurrent {

	enum class ParallelPolicyMode {
		PPM_FIXED_HEURISTIC,
		PPM_ADAPTIVE
	};

	enum class ThreadPoolSizeSource {
		TPSS_MANUAL,
		TPSS_AUTO_DETECTED,
		TPSS_AUTO_FALLBACK,
		TPSS_FORCED_SINGLE_THREADED
	};

	struct ThreadPoolHardwareInfo {
		size_t requestedThreadCount = 0;
		unsigned int hardwareThreadCount = 0;
		size_t resolvedThreadCount = 1;
		ThreadPoolSizeSource source = ThreadPoolSizeSource::TPSS_AUTO_FALLBACK;
	};

	struct ParallelAdaptivePolicyConfig {
		ParallelPolicyMode mode = ParallelPolicyMode::PPM_ADAPTIVE;
		uint32_t minSamplesBeforeRecommend = 2;
		double minImprovementRatio = 1.08;
		double ewmaAlpha = 0.25;
		bool explorationEnabled = true;
	};

	struct ParallelAdaptiveStats {
		uint32_t samples = 0;
		double throughputEwma = 0.0;
		double elapsedNsEwma = 0.0;
		double mergeRatioEwma = 0.0;
	};

	namespace detail {
		inline constexpr size_t kAdaptiveWorkloadCount = 5;
		inline constexpr size_t kAdaptiveSizeBucketCount = 9;
		inline constexpr size_t kAdaptiveWorkerSlotCount = 11; // 1..1024 by powers of two.

		inline size_t workloadIndex(ParallelWorkloadKind workloadKind) {
			switch (workloadKind) {
				case ParallelWorkloadKind::PWK_CPU_BOUND:
					return 1;
				case ParallelWorkloadKind::PWK_MEMORY_SCAN:
					return 2;
				case ParallelWorkloadKind::PWK_MEMORY_INTENSIVE:
					return 3;
				case ParallelWorkloadKind::PWK_STORAGE_SCAN:
					return 4;
				case ParallelWorkloadKind::PWK_GENERAL:
				default:
					return 0;
			}
		}

		inline size_t estimateMagnitude(const ParallelWorkEstimate &estimate) {
			if (estimate.estimatedBytes != 0) {
				return estimate.estimatedBytes;
			}
			if (estimate.estimatedItems != 0) {
				return estimate.estimatedItems;
			}
			return estimate.partitions;
		}

		inline size_t sizeBucket(const ParallelWorkEstimate &estimate) {
			size_t magnitude = estimateMagnitude(estimate);
			size_t bucket = 0;
			size_t threshold = 1024;
			while (magnitude > threshold && bucket + 1 < kAdaptiveSizeBucketCount) {
				threshold = threshold > (static_cast<size_t>(-1) / 4) ? static_cast<size_t>(-1) : threshold * 4;
				++bucket;
			}
			return bucket;
		}

		inline size_t workerSlot(size_t workerCount) {
			size_t slot = 0;
			size_t value = 1;
			while (value < workerCount && slot + 1 < kAdaptiveWorkerSlotCount) {
				value <<= 1;
				++slot;
			}
			return slot;
		}

		inline size_t workerCountForSlot(size_t slot) {
			return static_cast<size_t>(1) << std::min(slot, kAdaptiveWorkerSlotCount - 1);
		}

		inline size_t clampWorkerCandidate(size_t workerCount, size_t hardWorkerLimit) {
			if (hardWorkerLimit == 0) {
				return 1;
			}
			return std::max<size_t>(1, std::min(workerCount, hardWorkerLimit));
		}

		inline bool hasEnoughGranularity(const ParallelWorkEstimate &estimate, size_t workerCount) {
			if (workerCount <= 1) {
				return true;
			}
			if (estimate.partitions != 0 && estimate.partitions < workerCount) {
				return false;
			}
			if (estimate.minItemsPerWorker != 0 && estimate.estimatedItems != 0 &&
				estimate.estimatedItems / workerCount < estimate.minItemsPerWorker) {
				return false;
			}
			if (estimate.minBytesPerWorker != 0 && estimate.estimatedBytes != 0 &&
				estimate.estimatedBytes / workerCount < estimate.minBytesPerWorker) {
				return false;
			}
			return true;
		}

		inline double telemetryThroughput(const ParallelExecutionTelemetry &telemetry) {
			if (telemetry.elapsedNs == 0) {
				return 0.0;
			}
			const auto &estimate = telemetry.estimate;
			const size_t magnitude = estimateMagnitude(estimate);
			if (magnitude == 0) {
				return 0.0;
			}
			return static_cast<double>(magnitude) / static_cast<double>(telemetry.elapsedNs);
		}

		inline double mergeRatio(const ParallelExecutionTelemetry &telemetry) {
			if (telemetry.elapsedNs == 0 || telemetry.mergeNs == 0) {
				return 0.0;
			}
			return std::min(1.0, static_cast<double>(telemetry.mergeNs) / static_cast<double>(telemetry.elapsedNs));
		}

		inline double updateEwma(double previous, double observed, double alpha, uint32_t samples) {
			if (samples == 0) {
				return observed;
			}
			return previous * (1.0 - alpha) + observed * alpha;
		}
	} // namespace detail

	class AdaptiveParallelPolicyState {
	public:
		[[nodiscard]] ParallelPolicyMode mode() const {
			std::lock_guard lock(mutex_);
			return config_.mode;
		}

		void setMode(ParallelPolicyMode mode) {
			std::lock_guard lock(mutex_);
			config_.mode = mode;
		}

		[[nodiscard]] ParallelAdaptivePolicyConfig config() const {
			std::lock_guard lock(mutex_);
			return config_;
		}

		void setConfig(ParallelAdaptivePolicyConfig config) {
			if (config.ewmaAlpha <= 0.0 || config.ewmaAlpha > 1.0) {
				config.ewmaAlpha = 0.25;
			}
			if (config.minImprovementRatio < 1.0) {
				config.minImprovementRatio = 1.0;
			}
			std::lock_guard lock(mutex_);
			config_ = config;
		}

		void reset() {
			std::lock_guard lock(mutex_);
			entries_ = {};
		}

		[[nodiscard]] size_t recommendWorkerCount(const ParallelWorkEstimate &estimate,
												   size_t baselineWorkerCount,
												   size_t hardWorkerLimit) const {
			const size_t clampedBaseline = detail::clampWorkerCandidate(baselineWorkerCount, hardWorkerLimit);
			if (clampedBaseline <= 1 || hardWorkerLimit <= 1) {
				return clampedBaseline;
			}

			std::lock_guard lock(mutex_);
			if (config_.mode != ParallelPolicyMode::PPM_ADAPTIVE) {
				return clampedBaseline;
			}

			const auto workload = detail::workloadIndex(estimate.workloadKind);
			const auto bucket = detail::sizeBucket(estimate);
			const auto baselineSlot = detail::workerSlot(clampedBaseline);
			const auto &statsByWorker = entries_[workload][bucket];
			const auto &baselineStats = statsByWorker[baselineSlot];

			size_t bestWorker = clampedBaseline;
			double bestThroughput = baselineStats.throughputEwma;
			bool foundReliableBetterWorker = false;
			for (size_t slot = 1; slot < statsByWorker.size(); ++slot) {
				const size_t candidate = detail::clampWorkerCandidate(detail::workerCountForSlot(slot), hardWorkerLimit);
				if (candidate <= 1 || candidate > hardWorkerLimit || !detail::hasEnoughGranularity(estimate, candidate)) {
					continue;
				}
				const auto &stats = statsByWorker[slot];
				if (stats.samples < config_.minSamplesBeforeRecommend || stats.throughputEwma <= 0.0) {
					continue;
				}
				const bool baselineReliable = baselineStats.samples >= config_.minSamplesBeforeRecommend &&
											  baselineStats.throughputEwma > 0.0;
				const bool beatsBaseline = !baselineReliable ||
										   stats.throughputEwma >=
												   baselineStats.throughputEwma * config_.minImprovementRatio;
				if (beatsBaseline && stats.throughputEwma > bestThroughput) {
					bestThroughput = stats.throughputEwma;
					bestWorker = candidate;
					foundReliableBetterWorker = true;
				}
			}
			if (foundReliableBetterWorker) {
				return bestWorker;
			}

			if (!config_.explorationEnabled ||
				baselineStats.samples < config_.minSamplesBeforeRecommend) {
				return clampedBaseline;
			}

			for (size_t slot = baselineSlot + 1; slot < statsByWorker.size(); ++slot) {
				const size_t candidate = detail::clampWorkerCandidate(detail::workerCountForSlot(slot), hardWorkerLimit);
				if (candidate <= clampedBaseline || candidate > hardWorkerLimit ||
					!detail::hasEnoughGranularity(estimate, candidate)) {
					continue;
				}
				if (statsByWorker[slot].samples < config_.minSamplesBeforeRecommend) {
					return candidate;
				}
			}
			return clampedBaseline;
		}

		void record(const ParallelExecutionTelemetry &telemetry) {
			if (!telemetry.completed || telemetry.elapsedNs == 0 || telemetry.workerCount == 0) {
				return;
			}
			const double throughput = detail::telemetryThroughput(telemetry);
			if (throughput <= 0.0 || !std::isfinite(throughput)) {
				return;
			}

			const auto workload = detail::workloadIndex(telemetry.estimate.workloadKind);
			const auto bucket = detail::sizeBucket(telemetry.estimate);
			const auto slot = detail::workerSlot(telemetry.workerCount);
			std::lock_guard lock(mutex_);
			auto &stats = entries_[workload][bucket][slot];
			const double elapsed = static_cast<double>(telemetry.elapsedNs);
			const double merge = detail::mergeRatio(telemetry);
			stats.throughputEwma = detail::updateEwma(
					stats.throughputEwma, throughput, config_.ewmaAlpha, stats.samples);
			stats.elapsedNsEwma = detail::updateEwma(
					stats.elapsedNsEwma, elapsed, config_.ewmaAlpha, stats.samples);
			stats.mergeRatioEwma = detail::updateEwma(
					stats.mergeRatioEwma, merge, config_.ewmaAlpha, stats.samples);
			++stats.samples;
		}

		[[nodiscard]] ParallelAdaptiveStats statsFor(const ParallelWorkEstimate &estimate, size_t workerCount) const {
			std::lock_guard lock(mutex_);
			return entries_[detail::workloadIndex(estimate.workloadKind)]
						   [detail::sizeBucket(estimate)]
						   [detail::workerSlot(workerCount)];
		}

	private:
		using WorkerStats = std::array<ParallelAdaptiveStats, detail::kAdaptiveWorkerSlotCount>;
		using SizeStats = std::array<WorkerStats, detail::kAdaptiveSizeBucketCount>;
		using WorkloadStats = std::array<SizeStats, detail::kAdaptiveWorkloadCount>;

		mutable std::mutex mutex_;
		ParallelAdaptivePolicyConfig config_;
		WorkloadStats entries_{};
	};

} // namespace graph::concurrent
