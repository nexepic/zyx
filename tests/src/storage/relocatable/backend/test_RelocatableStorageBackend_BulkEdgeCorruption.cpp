/**
 * @file test_RelocatableStorageBackend_BulkEdgeCorruption.cpp
 * @brief Public fail-closed contracts for streamed bulk Edge scratch sources.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

#include <algorithm>
#include <atomic>
#include <span>

using namespace graph::storage;
using namespace relocatable_storage_backend_test;

namespace {

	constexpr size_t MANIFEST_RECORD_COUNT_OFFSET = 16U;
	constexpr size_t MANIFEST_LEVEL_OFFSET = 10U;
	constexpr size_t MANIFEST_REFERENCE_COUNT_OFFSET = 12U;
	constexpr size_t MANIFEST_REFERENCES_CRC_OFFSET = 56U;
	constexpr size_t MANIFEST_HEADER_CRC_OFFSET = 60U;
	constexpr size_t MANIFEST_REFERENCE_BYTES = 16U;
	constexpr size_t RUN_RECORD_COUNT_OFFSET = 12U;
	constexpr size_t RUN_RECORD_BYTES_OFFSET = 16U;
	constexpr size_t RUN_RECORDS_CRC_OFFSET = 20U;
	constexpr size_t RUN_HEADER_CRC_OFFSET = 32U;
	constexpr size_t RUN_FRAMED_RECORD_HEADER_BYTES = 8U;

	void resealManifestHeader(std::span<std::byte> manifest) {
		LittleEndianCodec::writeU32(manifest, MANIFEST_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(
				manifest, MANIFEST_HEADER_CRC_OFFSET,
				graph::utils::calculateCrc(manifest.data(), RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES));
	}

	[[nodiscard]] bool swapFirstTwoManifestReferences(std::span<std::byte> manifest) {
		if (manifest.size() < RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES ||
			LittleEndianCodec::readU16(manifest, MANIFEST_LEVEL_OFFSET) != 0U ||
			LittleEndianCodec::readU32(manifest, MANIFEST_REFERENCE_COUNT_OFFSET) < 2U) {
			return false;
		}
		const size_t referenceBytes =
				static_cast<size_t>(LittleEndianCodec::readU32(manifest, MANIFEST_REFERENCE_COUNT_OFFSET)) *
				MANIFEST_REFERENCE_BYTES;
		if (referenceBytes > manifest.size() - RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES) {
			return false;
		}
		auto references = manifest.subspan(RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES, referenceBytes);
		std::array<std::byte, MANIFEST_REFERENCE_BYTES> first{};
		std::copy_n(references.begin(), first.size(), first.begin());
		std::copy_n(references.begin() + static_cast<std::ptrdiff_t>(MANIFEST_REFERENCE_BYTES), first.size(),
					references.begin());
		std::copy(first.begin(), first.end(), references.begin() + static_cast<std::ptrdiff_t>(MANIFEST_REFERENCE_BYTES));
		LittleEndianCodec::writeU32(manifest, MANIFEST_REFERENCES_CRC_OFFSET,
				graph::utils::calculateCrc(references.data(), references.size()));
		resealManifestHeader(manifest);
		return true;
	}

	void resealSegmentPayload(SegmentEnvelope envelope, std::span<const std::byte> payload,
			std::span<std::byte> segment) {
		envelope.payload_crc = graph::utils::calculateCrc(payload.data(), payload.size());
		const auto resealedEnvelope = SegmentEnvelopeCodec::encode(envelope);
		std::copy(resealedEnvelope.begin(), resealedEnvelope.end(), segment.begin());
	}

	[[nodiscard]] bool swapFirstTwoRunRecords(std::span<std::byte> run) {
		if (run.size() < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			LittleEndianCodec::readU32(run, RUN_RECORD_COUNT_OFFSET) < 2U) {
			return false;
		}
		const size_t recordBytes = LittleEndianCodec::readU32(run, RUN_RECORD_BYTES_OFFSET);
		if (recordBytes > run.size() - RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES) {
			return false;
		}
		auto records = run.subspan(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES, recordBytes);
		if (records.size() < RUN_FRAMED_RECORD_HEADER_BYTES) {
			return false;
		}
		const size_t firstBytes = RUN_FRAMED_RECORD_HEADER_BYTES + LittleEndianCodec::readU32(records, 0U);
		if (firstBytes > records.size() || records.size() - firstBytes < RUN_FRAMED_RECORD_HEADER_BYTES) {
			return false;
		}
		const size_t secondBytes =
				RUN_FRAMED_RECORD_HEADER_BYTES + LittleEndianCodec::readU32(records, firstBytes);
		if (secondBytes > records.size() - firstBytes) {
			return false;
		}

		const std::vector<std::byte> first(records.begin(), records.begin() + static_cast<std::ptrdiff_t>(firstBytes));
		const std::vector<std::byte> second(records.begin() + static_cast<std::ptrdiff_t>(firstBytes),
														 records.begin() + static_cast<std::ptrdiff_t>(firstBytes + secondBytes));
		std::copy(second.begin(), second.end(), records.begin());
		std::copy(first.begin(), first.end(), records.begin() + static_cast<std::ptrdiff_t>(secondBytes));

		// Frame and record checksums remain valid after moving complete frames; reseal
		// the two enclosing run checksums so decoding reaches the ordering invariant.
		LittleEndianCodec::writeU32(run, RUN_RECORDS_CRC_OFFSET,
				graph::utils::calculateCrc(records.data(), records.size()));
		LittleEndianCodec::writeU32(run, RUN_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(run, RUN_HEADER_CRC_OFFSET,
				graph::utils::calculateCrc(run.data(), RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES));
		return true;
	}

} // namespace

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeAuthenticatedManifestCountDivergenceRollsBackCleansUpAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	// Avoid retaining the private manifest in the page cache before the hook can
	// exercise the authenticated publication read boundary.
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> oldEdge = backend.createLogicalEdges({{nodes[0], nodes[1], "PREEXISTING", {}}});
	ASSERT_EQ(oldEdge.size(), 1U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[1]};
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_COUNT_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 2U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting corruptManifestCount([&injected](file_handle_t fd, void *bytes,
																				 size_t byteCount, pwrite_off_t offset) {
			const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
			if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) || injected.load(std::memory_order_relaxed)) {
				return read;
			}
			try {
				auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
				const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
				if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
					envelope.header_bytes + RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES > segment.size()) {
					return read;
				}
				auto manifest = segment.subspan(envelope.header_bytes, RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
				if (LittleEndianCodec::readU32(manifest, 0U) != RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC) {
					return read;
				}
				const uint64_t recordCount = LittleEndianCodec::readU64(manifest, MANIFEST_RECORD_COUNT_OFFSET);
				if (recordCount < 2U) {
					return read;
				}
				// Preserve the manifest checksum so publication reaches the semantic
				// descendant-count check instead of merely rejecting a torn frame.
				LittleEndianCodec::writeU64(manifest, MANIFEST_RECORD_COUNT_OFFSET, recordCount - 1U);
				resealManifestHeader(manifest);
				injected.store(true, std::memory_order_relaxed);
			} catch (const FormatException &) {
				// Non-scratch segments are irrelevant to this targeted source mutation.
			}
			return read;
		});
		EXPECT_THROW(static_cast<void>(transaction->commit()), FormatException);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdge.front()), LogicalGraphEdge(oldEdge.front(), nodes[0], nodes[1], "PREEXISTING"));
	for (const uint64_t edgeId: rejected) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retried =
			retryBulk->appendPersistedBulkEdges({"BULK_COUNT_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(retried.size(), 2U);
	EXPECT_GT(retried.front(), rejected.back());
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	EXPECT_EQ(retry->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	retry.reset();

	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdge.front()), LogicalGraphEdge(oldEdge.front(), nodes[0], nodes[1], "PREEXISTING"));
	for (const uint64_t edgeId: retried) {
		EXPECT_EQ(snapshot->readEdge(edgeId),
				  LogicalGraphEdge(edgeId, nodes[0], nodes[1], "BULK_COUNT_DIVERGENCE"));
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeRebasedManifestCountDivergenceRetainsExternalGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	// Keep the manifest out of the stale handle's cache so the rebase reaches the
	// authenticated source-read boundary after observing the external publication.
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[1]};
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_REBASED_COUNT_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 2U);

	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend concurrentWriter(std::move(writerOptions));
	concurrentWriter.open();
	const std::vector<uint64_t> external =
			concurrentWriter.createLogicalEdges({{nodes[0], nodes[1], "BULK_REBASED_EXTERNAL", {}}});
	ASSERT_EQ(external.size(), 1U);
	concurrentWriter.close();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting corruptRebasedManifestCount(
				[&injected](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
					if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) ||
						injected.load(std::memory_order_relaxed)) {
						return read;
					}
					try {
						auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
						const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
						if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
							envelope.header_bytes + RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES > segment.size()) {
							return read;
						}
						auto manifest = segment.subspan(envelope.header_bytes, RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES);
						const uint64_t recordCount = LittleEndianCodec::readU64(manifest, MANIFEST_RECORD_COUNT_OFFSET);
						if (LittleEndianCodec::readU32(manifest, 0U) != RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC ||
							recordCount < 2U) {
							return read;
						}
						// Preserve the wire checksum so publication rejects the authenticated
						// semantic count mismatch rather than a torn source frame.
						LittleEndianCodec::writeU64(manifest, MANIFEST_RECORD_COUNT_OFFSET, recordCount - 1U);
						resealManifestHeader(manifest);
						injected.store(true, std::memory_order_relaxed);
					} catch (const FormatException &) {
						// Non-scratch segments are irrelevant to this targeted source mutation.
					}
					return read;
				});
		EXPECT_THROW(static_cast<void>(transaction->commit()), FormatException);
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "BULK_REBASED_EXTERNAL"));
	for (const uint64_t edgeId: rejected) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retried =
			retryBulk->appendPersistedBulkEdges({"BULK_REBASED_COUNT_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(retried.size(), 2U);
	EXPECT_GT(retried.front(), rejected.back());
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	EXPECT_EQ(retry->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	retry.reset();

	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "BULK_REBASED_EXTERNAL"));
	for (const uint64_t edgeId: retried) {
		EXPECT_EQ(snapshot->readEdge(edgeId),
				  LogicalGraphEdge(edgeId, nodes[0], nodes[1], "BULK_REBASED_COUNT_DIVERGENCE"));
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeRebasedRunOrderingDivergenceRetainsExternalGraphAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[1]};
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_REBASED_ORDER_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 2U);

	RelocatableStorageBackendOptions writerOptions = backendOptions();
	writerOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend concurrentWriter(std::move(writerOptions));
	concurrentWriter.open();
	const std::vector<uint64_t> external =
			concurrentWriter.createLogicalEdges({{nodes[0], nodes[1], "BULK_REBASED_EXTERNAL", {}}});
	ASSERT_EQ(external.size(), 1U);
	concurrentWriter.close();

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting reorderRebasedRunRecords(
				[&injected](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
					if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) ||
						injected.load(std::memory_order_relaxed)) {
						return read;
					}
					try {
						auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
						const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
						if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
							envelope.header_bytes > segment.size() ||
							envelope.payload_bytes > segment.size() - envelope.header_bytes ||
							envelope.payload_bytes < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES) {
							return read;
						}
						auto payload = segment.subspan(envelope.header_bytes, envelope.payload_bytes);
						if (LittleEndianCodec::readU32(payload, 0U) == RSS_NATIVE_BULK_EDGE_RUN_MAGIC &&
							swapFirstTwoRunRecords(payload)) {
							resealSegmentPayload(envelope, payload, segment);
							injected.store(true, std::memory_order_relaxed);
						}
					} catch (const FormatException &) {
						// Non-scratch segments are irrelevant to this targeted source mutation.
					}
					return read;
				});
		try {
			static_cast<void>(transaction->commit());
			ADD_FAILURE() << "authenticated unordered rebased source run unexpectedly committed";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER);
			EXPECT_STREQ(error.what(), "native bulk Edge run records are not strictly ordered by Edge ID");
		}
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "BULK_REBASED_EXTERNAL"));
	for (const uint64_t edgeId: rejected) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = backend.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retried =
			retryBulk->appendPersistedBulkEdges({"BULK_REBASED_ORDER_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(retried.size(), 2U);
	EXPECT_GT(retried.front(), rejected.back());
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	EXPECT_EQ(retry->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	retry.reset();

	snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(external.front()),
			  LogicalGraphEdge(external.front(), nodes[0], nodes[1], "BULK_REBASED_EXTERNAL"));
	for (const uint64_t edgeId: retried) {
		EXPECT_EQ(snapshot->readEdge(edgeId),
				  LogicalGraphEdge(edgeId, nodes[0], nodes[1], "BULK_REBASED_ORDER_DIVERGENCE"));
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeAuthenticatedRunOrderingDivergenceRollsBackCleansUpAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	// Avoid retaining the private run in the page cache before the hook can
	// exercise the authenticated publication read boundary.
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> oldEdge = backend.createLogicalEdges({{nodes[0], nodes[1], "PREEXISTING", {}}});
	ASSERT_EQ(oldEdge.size(), 1U);

	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::array<uint64_t, 2U> sources{nodes[0], nodes[0]};
	const std::array<uint64_t, 2U> targets{nodes[1], nodes[1]};
	const std::vector<uint64_t> rejected =
			bulk->appendPersistedBulkEdges({"BULK_ORDER_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(rejected.size(), 2U);

	std::atomic<bool> injected{false};
	{
		StorageIO::ScopedNativeReadHookForTesting reorderRunRecords([&injected](file_handle_t fd, void *bytes,
																							 size_t byteCount, pwrite_off_t offset) {
			const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
			if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) || injected.load(std::memory_order_relaxed)) {
				return read;
			}
			try {
				auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
				const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
				if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
					envelope.header_bytes > segment.size() || envelope.payload_bytes > segment.size() - envelope.header_bytes ||
					envelope.payload_bytes < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES) {
					return read;
				}
				auto payload = segment.subspan(envelope.header_bytes, envelope.payload_bytes);
				if (LittleEndianCodec::readU32(payload, 0U) == RSS_NATIVE_BULK_EDGE_RUN_MAGIC &&
					swapFirstTwoRunRecords(payload)) {
					resealSegmentPayload(envelope, payload, segment);
					injected.store(true, std::memory_order_relaxed);
				}
			} catch (const FormatException &) {
				// Non-scratch segments are irrelevant to this targeted source mutation.
			}
			return read;
		});
		try {
			static_cast<void>(transaction->commit());
			ADD_FAILURE() << "authenticated unordered source run unexpectedly committed";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), FormatError::FERR_INVALID_HEADER);
			EXPECT_STREQ(error.what(), "native bulk Edge run records are not strictly ordered by Edge ID");
		}
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdge.front()), LogicalGraphEdge(oldEdge.front(), nodes[0], nodes[1], "PREEXISTING"));
	for (const uint64_t edgeId: rejected) {
		EXPECT_THROW(static_cast<void>(snapshot->readEdge(edgeId)), FormatException);
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retried =
			retryBulk->appendPersistedBulkEdges({"BULK_ORDER_DIVERGENCE", sources, targets, {}});
	ASSERT_EQ(retried.size(), 2U);
	EXPECT_GT(retried.front(), rejected.back());
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	EXPECT_EQ(retry->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	retry.reset();

	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdge.front()), LogicalGraphEdge(oldEdge.front(), nodes[0], nodes[1], "PREEXISTING"));
	for (const uint64_t edgeId: retried) {
		EXPECT_EQ(snapshot->readEdge(edgeId),
				  LogicalGraphEdge(edgeId, nodes[0], nodes[1], "BULK_ORDER_DIVERGENCE"));
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}

TEST_F(RelocatableStorageBackendTest,
		StreamedBulkEdgeAuthenticatedCrossRunOrderingDivergenceRollsBackCleansUpAndAllowsRetry) {
	materializeNativeDatabase();
	RelocatableStorageBackendOptions options = backendOptions();
	options.open_mode = OpenMode::OPEN_EXISTING_FILE;
	options.maximum_bulk_edge_transaction_records = 1'800U;
	options.maximum_bulk_edge_append_records = 1'800U;
	// Avoid retaining either source run or its root manifest before the hook can
	// exercise the authenticated publication read boundary.
	options.buffer_pool_bytes = RSS_BUFFER_POOL_PAGE_BYTES;
	RelocatableStorageBackend backend(std::move(options));
	backend.open();
	const std::vector<uint64_t> nodes = backend.createLogicalNodes({{{"BulkSource"}, {}}, {{"BulkTarget"}, {}}});
	ASSERT_EQ(nodes.size(), 2U);
	const std::vector<uint64_t> oldEdge = backend.createLogicalEdges({{nodes[0], nodes[1], "PREEXISTING", {}}});
	ASSERT_EQ(oldEdge.size(), 1U);

	std::vector<uint64_t> sources(1'800U, nodes[0]);
	std::vector<uint64_t> targets(1'800U, nodes[1]);
	std::unique_ptr<LogicalGraphTransaction> transaction = backend.beginLogicalGraphTransaction(false);
	auto *const bulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(transaction.get());
	ASSERT_NE(bulk, nullptr);
	const std::vector<uint64_t> rejected = bulk->appendPersistedBulkEdges({"O", sources, targets, {}});
	ASSERT_EQ(rejected.size(), sources.size());

	std::atomic<bool> injected{false};
	std::atomic<uint32_t> rootManifestReads{0U};
	{
		StorageIO::ScopedNativeReadHookForTesting reorderManifestRuns([&injected, &rootManifestReads](file_handle_t fd,
																												  void *bytes, size_t byteCount, pwrite_off_t offset) {
			const pwrite_ssize_t read = portable_pread(fd, bytes, byteCount, offset);
			if (read != static_cast<pwrite_ssize_t>(RSS_SEGMENT_SIZE) || injected.load(std::memory_order_relaxed)) {
				return read;
			}
			try {
				auto segment = std::span<std::byte>(static_cast<std::byte *>(bytes), byteCount);
				const SegmentEnvelope envelope = SegmentEnvelopeCodec::decode(segment.first(RSS_ENVELOPE_BYTES));
				if (envelope.kind != SegmentKind::SGK_BULK_SCRATCH ||
					envelope.header_bytes > segment.size() || envelope.payload_bytes > segment.size() - envelope.header_bytes ||
					envelope.payload_bytes < RSS_NATIVE_BULK_EDGE_MANIFEST_HEADER_BYTES) {
					return read;
				}
				auto payload = segment.subspan(envelope.header_bytes, envelope.payload_bytes);
				if (LittleEndianCodec::readU32(payload, 0U) == RSS_NATIVE_BULK_EDGE_MANIFEST_MAGIC &&
					rootManifestReads.fetch_add(1U, std::memory_order_relaxed) + 1U == 2U &&
					swapFirstTwoManifestReferences(payload)) {
					resealSegmentPayload(envelope, payload, segment);
					injected.store(true, std::memory_order_relaxed);
				}
			} catch (const FormatException &) {
				// Non-scratch segments are irrelevant to this targeted source mutation.
			}
			return read;
		});
		try {
			static_cast<void>(transaction->commit());
			ADD_FAILURE() << "authenticated cross-run source ordering unexpectedly committed";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE);
			EXPECT_STREQ(error.what(), "native bulk Edge source run ordering is inconsistent with its manifest");
		}
	}
	EXPECT_TRUE(injected.load(std::memory_order_relaxed));
	EXPECT_GE(rootManifestReads.load(std::memory_order_relaxed), 2U);
	EXPECT_EQ(transaction->state(), LogicalGraphTransactionState::LGTTS_ROLLED_BACK);
	transaction.reset();

	std::unique_ptr<LogicalGraphReadSnapshot> snapshot = backend.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdge.front()), LogicalGraphEdge(oldEdge.front(), nodes[0], nodes[1], "PREEXISTING"));
	size_t unexpectedlyVisible = 0U;
	for (const uint64_t edgeId: rejected) {
		try {
			static_cast<void>(snapshot->readEdge(edgeId));
			++unexpectedlyVisible;
		} catch (const FormatException &) {
		}
	}
	EXPECT_EQ(unexpectedlyVisible, 0U);
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity()));
	backend.close();

	RelocatableStorageBackendOptions reopenOptions = backendOptions();
	reopenOptions.open_mode = OpenMode::OPEN_EXISTING_FILE;
	RelocatableStorageBackend reopened(std::move(reopenOptions));
	EXPECT_NO_THROW(reopened.open());
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));

	std::unique_ptr<LogicalGraphTransaction> retry = reopened.beginLogicalGraphTransaction(false);
	auto *const retryBulk = dynamic_cast<LogicalGraphBoundedBulkEdgeTransaction *>(retry.get());
	ASSERT_NE(retryBulk, nullptr);
	const std::vector<uint64_t> retried = retryBulk->appendPersistedBulkEdges({"O", sources, targets, {}});
	ASSERT_EQ(retried.size(), sources.size());
	EXPECT_GT(retried.front(), rejected.back());
	EXPECT_NO_THROW(static_cast<void>(retry->commit()));
	EXPECT_EQ(retry->state(), LogicalGraphTransactionState::LGTTS_COMMITTED);
	retry.reset();

	snapshot = reopened.openLogicalGraphReadSnapshot();
	EXPECT_EQ(snapshot->readEdge(oldEdge.front()), LogicalGraphEdge(oldEdge.front(), nodes[0], nodes[1], "PREEXISTING"));
	for (const uint64_t edgeId: retried) {
		EXPECT_EQ(snapshot->readEdge(edgeId), LogicalGraphEdge(edgeId, nodes[0], nodes[1], "O"));
	}
	snapshot.reset();
	EXPECT_NO_THROW(static_cast<void>(reopened.verifyIntegrity()));
	reopened.close();
}
