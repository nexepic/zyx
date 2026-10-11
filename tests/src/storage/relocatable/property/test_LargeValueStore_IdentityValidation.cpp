/**
 * @file test_LargeValueStore_IdentityValidation.cpp
 * @brief LargeValue logical-identity boundary tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>

#include "graph/storage/relocatable/property/LargeValueStore.hpp"

namespace {

	[[nodiscard]] LargeValueWriteOptions largeValueWriteOptions() {
		LargeValueWriteOptions result;
		result.first_data_slot = PhysicalSlotId(40U);
		result.last_data_slot = PhysicalSlotId(80U);
		result.first_manifest_slot = PhysicalSlotId(81U);
		result.last_manifest_slot = PhysicalSlotId(90U);
		result.kind = LargeValueKind::LVK_BYTES;
		result.media_type = "application/octet-stream";
		return result;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, LargeValueStoreRejectsWritesAfterDurableIdentityExhaustion) {
	Superblock exhausted = superblock(1U, 1U);
	// Zero is the persisted representation for an exhausted, never-reused
	// LargeValue identity namespace.
	exhausted.next_large_value_id = 0U;
	auto metadata = RelocatableMetadataStore::initializeNew(io, exhausted);
	LargeValueStore store(io, *metadata);
	const std::array input{std::byte{0x42U}};
	MetadataSnapshotPin before = metadata->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	expectFormatError(
			[&] { static_cast<void>(store.write(input, largeValueWriteOptions(), LogSequenceNumber(2U))); },
			FormatError::FERR_ARITHMETIC_OVERFLOW);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(snapshot.superblock(), beforeSuperblock);
	EXPECT_FALSE(metadata->findLargeValueManifestOptional(snapshot, LargeValueId(1U)).has_value());
}

TEST_F(RelocatableMetadataStoreTest, LargeValueFullReadAdmitsPayloadBeforeAllocationAndRetries) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::vector<std::byte> input(8192U, std::byte{0x42U});
	const auto written = store.write(input, largeValueWriteOptions(), LogSequenceNumber(2U));
	auto snapshot = metadata->openSnapshot();
	const auto root = store.manifest(snapshot, written.reference);
	const size_t rootBytes = root.media_type.capacity() + root.chunks.capacity() * sizeof(LargeValueChunk)
			+ root.children.capacity() * sizeof(LargeValueManifestChild);
	size_t requested = 0U;
	EXPECT_THROW(static_cast<void>(store.read(snapshot, written.reference, [&](size_t bytes) {
		requested = bytes; throw std::length_error("payload budget rejected");
	})), std::length_error);
	EXPECT_EQ(requested, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	EXPECT_THROW(static_cast<void>(store.read(snapshot, written.reference, [&](size_t bytes) {
		if (bytes > input.size()) throw std::length_error("chunk budget rejected");
	})), std::length_error);
	EXPECT_THROW(static_cast<void>(store.read(snapshot, written.reference, [&](size_t bytes) {
		if (bytes > 2U * input.size() + rootBytes) throw std::length_error("data page buffers rejected");
	})), std::length_error);
	for (int retry = 0; retry < 2; ++retry) {
		size_t peak = 0U;
		const auto output = store.read(snapshot, written.reference, [&](size_t bytes) { requested = bytes; peak = std::max(peak, bytes); });
		EXPECT_GT(peak, output.capacity());
		EXPECT_LE(peak, output.capacity() + input.size() + rootBytes + 2U * RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		EXPECT_GT(peak, output.capacity() + rootBytes + RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		EXPECT_EQ(output, input);
		EXPECT_EQ(requested, output.capacity());
	}
}

TEST_F(RelocatableMetadataStoreTest, LargeValueManifestRangeRejectsForeignAndReleasedSnapshotsBeforeAdmission) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::array input{std::byte{0x42U}, std::byte{0x43U}};
	const auto written = store.write(input, largeValueWriteOptions(), LogSequenceNumber(2U));
	auto snapshot = metadata->openSnapshot();
	const auto manifest = store.manifest(snapshot, written.reference);
	auto otherMetadata = RelocatableMetadataStore::openReadOnly(io);
	auto foreign = otherMetadata->openSnapshot();
	size_t calls = 0U;
	const auto rejectAdmission = [&](size_t) { ++calls; throw std::length_error("admission must not run"); };
	for (uint64_t length: {0U, 1U}) {
		expectFormatError([&] { static_cast<void>(store.readRangeFromManifest(foreign, manifest, 0U, length, rejectAdmission)); },
			FormatError::FERR_INVALID_STATE);
		EXPECT_EQ(calls, 0U);
	}
	foreign.release(); snapshot.release();
	for (uint64_t length: {0U, 1U}) {
		expectFormatError([&] { static_cast<void>(store.readRangeFromManifest(snapshot, manifest, 0U, length, rejectAdmission)); },
			FormatError::FERR_INVALID_STATE);
		EXPECT_EQ(calls, 0U);
	}
	auto retry = metadata->openSnapshot();
	for (size_t repeat = 0U; repeat < 2U; ++repeat) {
		EXPECT_TRUE(store.readRangeFromManifest(retry, manifest, 0U, 0U).empty());
		EXPECT_EQ(store.readRangeFromManifest(retry, manifest, 0U, input.size()), std::vector<std::byte>(input.begin(), input.end()));
	}
}

TEST_F(RelocatableMetadataStoreTest, LargeValueChildManifestAdmissionIncludesDecodedArraysAndAllowsRetry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	auto options = largeValueWriteOptions();
	options.extent_class = LargeValueExtentClass::LVEC_16K;
	const size_t inputBytes = (static_cast<size_t>(RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) + 1U) *
			RSS_LARGE_VALUE_EXTENT_16K_BYTES;
	const std::vector<std::byte> input(inputBytes, std::byte{0x42U});
	const auto written = store.write(input, options, LogSequenceNumber(2U));
	auto snapshot = metadata->openSnapshot();
	const auto root = store.manifest(snapshot, written.reference);
	ASSERT_FALSE(root.children.empty());
	const size_t limit = 1U + RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	for (int retry = 0; retry < 2; ++retry) {
		size_t rejected = 0U;
		EXPECT_THROW(static_cast<void>(store.readRangeFromManifest(snapshot, root, 0U, 1U,
				[&](size_t bytes) {
					if (bytes > limit) { rejected = bytes; throw std::length_error("child manifest budget"); }
				})), std::length_error);
		EXPECT_GT(rejected, limit);
		size_t peak = 0U;
		const auto output = store.readRangeFromManifest(snapshot, root, 0U, 1U,
				[&](size_t bytes) { peak = std::max(peak, bytes); });
		EXPECT_EQ(output, std::vector<std::byte>{std::byte{0x42U}});
		EXPECT_GT(peak, limit);
	}
}

TEST_F(RelocatableMetadataStoreTest, LargeValueRootManifestAdmitsPageAndDecodedArraysBeforeRetry) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	LargeValueStore store(io, *metadata);
	const std::array input{std::byte{0x42U}};
	const auto written = store.write(input, largeValueWriteOptions(), LogSequenceNumber(2U));
	auto snapshot = metadata->openSnapshot();
	size_t first = 0U;
	EXPECT_THROW(static_cast<void>(store.manifest(snapshot, written.reference, [&](size_t bytes) {
		first = bytes; throw std::length_error("root page budget");
	})), std::length_error);
	EXPECT_EQ(first, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	for (int retry = 0; retry < 2; ++retry) {
		EXPECT_THROW(static_cast<void>(store.manifest(snapshot, written.reference, [](size_t bytes) {
			if (bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) throw std::length_error("root arrays budget");
		})), std::length_error);
		size_t peak = 0U;
		const auto decoded = store.manifest(snapshot, written.reference,
				[&](size_t bytes) { peak = std::max(peak, bytes); });
		EXPECT_EQ(decoded, written.manifest);
		EXPECT_GT(peak, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	}
}
