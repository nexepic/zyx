#include <gtest/gtest.h>
#include "benchmarks/graphdb-comparison/property-block-prototype/SparseFixedWidthPropertyColumn.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/AdaptivePropertyColumn.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/PackedPropertyColumnPage.hpp"
#include "benchmarks/graphdb-comparison/property-block-prototype/CompactFixedPropertyColumn.hpp"

using zyx::benchmark::SparseFixedWidthPropertyColumn;
using graph::PropertyValue;
using graph::storage::LittleEndianCodec;

TEST(SparseFixedWidthPropertyColumnPrototypeTest, VerifiedCompactColumnReadsRankWithoutBorrowingMutableInput) {
	using Compact = zyx::benchmark::CompactFixedPropertyColumn;
	for (const auto &value : {PropertyValue(int64_t(-9)), PropertyValue(-0.0), PropertyValue(true), PropertyValue{}}) {
		std::vector<std::optional<PropertyValue>> values(64U);
		values[0U] = PropertyValue{}; values[17U] = value; values[63U] = value;
		auto bytes = Compact::encode(values);
		const auto verified = Compact::freeze(bytes);
		std::fill(bytes.begin(), bytes.end(), std::byte{});
		for (size_t row = 0U; row < values.size(); ++row) EXPECT_EQ(verified.read(row), values[row]);
		EXPECT_THROW(static_cast<void>(verified.read(64U)), std::out_of_range);
	}
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPageReadsSelectedKindsAndExplicitlyMigratesReplacement) {
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	using Compact = zyx::benchmark::CompactFixedPropertyColumn;
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	const std::vector<std::optional<PropertyValue>> integer{PropertyValue(int64_t(1)), std::nullopt, PropertyValue{}};
	const std::vector<std::optional<PropertyValue>> mixed{PropertyValue("changed"), std::nullopt, PropertyValue{}};
	const auto compact = Compact::encode(integer);
	const auto adaptive = Column::encode(mixed, 1000U).encodeImage(1000U);
	const std::vector<Page::Column> columns{{2U, compact, Page::Kind::PCP_COMPACT_FIXED}, {9U, adaptive}};
	const auto before = Page::encode(columns, 1000U);
	EXPECT_EQ(Page::readColumn(before, 2U, 1000U), std::optional(integer));
	EXPECT_EQ(Page::readColumn(before, 9U, 1000U), std::optional(mixed));
	EXPECT_FALSE(Page::readColumn(before, 3U, 1000U));
	const auto changed = Page::replace(before, 2U, std::span<const std::byte>(adaptive), 1000U, 1U);
	ASSERT_EQ(changed.size(), 1U);
	EXPECT_EQ(Page::readColumn(changed[0U], 2U, 1000U), std::optional(mixed));
	const auto restored = Page::replace(changed[0U], 2U, std::span<const std::byte>(compact), 1000U, 1U,
	    nullptr, Page::Kind::PCP_COMPACT_FIXED);
	EXPECT_EQ(Page::readColumn(restored[0U], 2U, 1000U), std::optional(integer));
	EXPECT_EQ(Page::readColumn(before, 2U, 1000U), std::optional(integer));
	EXPECT_THROW(static_cast<void>(Page::replace(before, 2U, std::span<const std::byte>(adaptive), 1000U, 1U,
	    nullptr, Page::Kind::PCP_COMPACT_FIXED)), std::runtime_error);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPageTagsCompactAndAdaptiveChildrenAndReadsLegacy) {
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	using Compact = zyx::benchmark::CompactFixedPropertyColumn;
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(int64_t(9))};
	const auto compact = Compact::encode(values);
	const auto adaptive = Column::encode(values, 1000U).encodeImage(1000U);
	const std::vector<Page::Column> columns{{1U, compact, Page::Kind::PCP_COMPACT_FIXED}, {2U, adaptive}};
	const auto page = Page::encode(columns, 1000U);
	const auto decoded = Page::decode(page, 1000U);
	ASSERT_EQ(decoded.size(), 2U);
	EXPECT_EQ(decoded[0U].kind, Page::Kind::PCP_COMPACT_FIXED);
	EXPECT_EQ(Compact::readAll(decoded[0U].image), values);
	EXPECT_EQ(decoded[1U].kind, Page::Kind::PCP_ADAPTIVE);
	auto invalid = page; LittleEndianCodec::writeU32(invalid, Page::HEADER + 12U, 3U);
	LittleEndianCodec::writeU32(invalid, 16U, 0U);
	LittleEndianCodec::writeU32(invalid, 16U, graph::utils::calculateCrc(invalid.data(), invalid.size()));
	EXPECT_THROW(static_cast<void>(Page::decode(invalid, 1000U)), std::runtime_error);
	const std::vector<Page::Column> single{{2U, adaptive}};
	auto legacy = Page::encode(single, 1000U);
	legacy.erase(legacy.begin() + Page::HEADER + 12U, legacy.begin() + Page::HEADER + 16U);
	LittleEndianCodec::writeU32(legacy, 4U, 1U);
	LittleEndianCodec::writeU32(legacy, 12U, static_cast<uint32_t>(legacy.size()));
	LittleEndianCodec::writeU32(legacy, Page::HEADER + 4U, Page::HEADER + 12U);
	LittleEndianCodec::writeU32(legacy, 16U, 0U);
	LittleEndianCodec::writeU32(legacy, 16U, graph::utils::calculateCrc(legacy.data(), legacy.size()));
	EXPECT_EQ(Page::decode(legacy, 1000U)[0U].kind, Page::Kind::PCP_ADAPTIVE);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, CompactSubblocksPreserveTypesAndRejectMalformedMasks) {
	using Compact = zyx::benchmark::CompactFixedPropertyColumn;
	for (const auto &value : {PropertyValue(int64_t(-9)), PropertyValue(-0.0), PropertyValue(true), PropertyValue{}}) {
		std::vector<std::optional<PropertyValue>> values(64U);
		values[0U] = PropertyValue{}; values[63U] = value;
		const auto compact = Compact::encode(values);
		EXPECT_EQ(Compact::readAll(compact), values);
		EXPECT_EQ(compact.size(), Compact::HEADER + (std::holds_alternative<std::monostate>(value.getVariant()) ? 0U :
		    std::holds_alternative<bool>(value.getVariant()) ? 1U : 8U));
		for (int mode = 0; mode < 4; ++mode) {
			auto invalid = compact;
			if (mode == 0) LittleEndianCodec::writeU16(invalid, 0U, 5U);
			if (mode == 1) LittleEndianCodec::writeU16(invalid, 2U, 65U);
			if (mode == 2) LittleEndianCodec::writeU32(invalid, 4U, 1U);
			if (mode == 3) LittleEndianCodec::writeU64(invalid, 16U, UINT64_MAX);
			EXPECT_THROW(static_cast<void>(Compact::readAll(invalid)), std::runtime_error);
		}
	}
	EXPECT_TRUE(Compact::readAll(Compact::encode({})).empty());
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, LargeSharedPageCancelsDuringCopyAndChecksum) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	struct Checkpoint final : graph::storage::CancellationCheckpoint {
		mutable size_t calls = 0U; size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(std::string(150000U, 'x'))};
	const auto image = Column::encode(values, 200000U).encodeImage(200000U);
	const std::vector<Page::Column> columns{{1U, image}};
	Checkpoint successful;
	const auto page = Page::encode(columns, 200000U, &successful);
	for (size_t stop = 1U; stop <= successful.calls; ++stop) {
		Checkpoint cancelled; cancelled.stop = stop;
		EXPECT_THROW(static_cast<void>(Page::encode(columns, 200000U, &cancelled)), std::runtime_error);
		EXPECT_EQ(cancelled.calls, stop);
	}
	const auto decoded = Page::decode(page, 200000U);
	ASSERT_EQ(decoded.size(), 1U);
	EXPECT_EQ(Column::decodeImage(decoded[0U].image, 200000U).read(0U), values[0U]);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPageCountAdmissionPrecedesPayloadDecoding) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(int64_t(1))};
	auto image = Column::encode(values, 1000U).encodeImage(1000U);
	image.back() ^= std::byte{1U};
	const std::vector<Page::Column> columns{{1U, image}, {2U, image}};
	const size_t capacity = Page::HEADER + Page::ENTRY + image.size();
	EXPECT_THROW(static_cast<void>(Page::pack(columns, capacity, 1U)), std::length_error);
	EXPECT_THROW(static_cast<void>(Page::pack(columns, capacity, 2U)), std::runtime_error);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPageOperationsCancelWithoutChangingInputs) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	struct Checkpoint final : graph::storage::CancellationCheckpoint {
		mutable size_t calls = 0U; size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(int64_t(1))};
	const auto image = Column::encode(values, 1000U).encodeImage(1000U);
	const std::vector<Page::Column> columns{{1U, image}, {2U, image}};
	const auto page = Page::encode(columns, 1000U);
	const auto saved = page;
	for (int operation = 0; operation < 3; ++operation) {
		auto run = [&](Checkpoint &checkpoint) {
			if (operation == 0) static_cast<void>(Page::pack(columns, 1000U, 2U, &checkpoint));
			if (operation == 1) static_cast<void>(Page::decode(page, 1000U, &checkpoint));
			if (operation == 2) static_cast<void>(Page::replace(page, 1U, std::nullopt, 1000U, 2U, &checkpoint));
		};
		Checkpoint count; run(count);
		for (size_t stop = 1U; stop <= count.calls; ++stop) {
			Checkpoint cancelled; cancelled.stop = stop;
			EXPECT_THROW(run(cancelled), std::runtime_error);
			EXPECT_EQ(cancelled.calls, stop);
			EXPECT_EQ(page, saved);
		}
	}
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPageReplacementSplitsLocallyAndPreservesOldPage) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	const std::vector<std::optional<PropertyValue>> small{PropertyValue(int64_t(1))}, large{PropertyValue(std::string(300U, 'x'))};
	const auto a = Column::encode(small, 1000U).encodeImage(1000U);
	const auto b = Column::encode(large, 1000U).encodeImage(1000U);
	const std::vector<Page::Column> columns{{1U, a}, {2U, a}, {3U, a}};
	const size_t capacity = Page::HEADER + Page::ENTRY + b.size();
	ASSERT_GE(capacity, Page::HEADER + columns.size() * (Page::ENTRY + a.size()));
	const auto before = Page::encode(columns, capacity);
	const auto saved = before;
	const auto changed = Page::replace(before, 2U, std::span<const std::byte>(b), capacity, 3U);
	ASSERT_EQ(changed.size(), 3U);
	EXPECT_EQ(Page::decode(changed[0U], capacity)[0U].token, 1U);
	EXPECT_EQ(Column::decodeImage(Page::decode(changed[1U], capacity)[0U].image, 1000U).read(0U), large[0U]);
	EXPECT_EQ(Page::decode(changed[2U], capacity)[0U].token, 3U);
	EXPECT_THROW(static_cast<void>(Page::replace(before, 2U, std::span<const std::byte>(b), capacity, 2U)), std::length_error);
	EXPECT_EQ(before, saved);
	const auto removed = Page::replace(before, 2U, std::nullopt, capacity, 1U);
	ASSERT_EQ(removed.size(), 1U);
	EXPECT_EQ(Page::decode(removed[0U], capacity).size(), 2U);
	const auto one = Page::encode(std::span<const Page::Column>(columns).first(1U), capacity);
	EXPECT_TRUE(Page::replace(one, 1U, std::nullopt, capacity, 1U).empty());
	EXPECT_THROW(static_cast<void>(Page::replace(before, 4U, std::nullopt, capacity, 1U)), std::invalid_argument);
	EXPECT_EQ(before, saved);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPagePackingHonorsExactCapacityAndPageCount) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(int64_t(1))};
	const auto image = Column::encode(values, 1000U).encodeImage(1000U);
	std::vector<Page::Column> columns;
	for (uint32_t token = 1U; token <= 5U; ++token) columns.push_back({token, image});
	const size_t pageBytes = Page::HEADER + 2U * (Page::ENTRY + image.size());
	const auto pages = Page::pack(columns, pageBytes, 3U);
	ASSERT_EQ(pages.size(), 3U);
	EXPECT_EQ(pages[0U].size(), pageBytes);
	EXPECT_EQ(pages[1U].size(), pageBytes);
	EXPECT_EQ(pages[2U].size(), Page::HEADER + Page::ENTRY + image.size());
	uint32_t expected = 1U;
	for (const auto &page : pages) for (const auto &column : Page::decode(page, pageBytes)) EXPECT_EQ(column.token, expected++);
	EXPECT_EQ(Page::pack(columns, pageBytes, 3U), pages);
	EXPECT_THROW(static_cast<void>(Page::pack(columns, pageBytes, 2U)), std::length_error);
	EXPECT_THROW(static_cast<void>(Page::pack(columns, Page::HEADER + Page::ENTRY + image.size() - 1U, 5U)), std::length_error);
	EXPECT_TRUE(Page::pack({}, pageBytes, 1U).empty());
	EXPECT_THROW(static_cast<void>(Page::pack(columns, pageBytes, 0U)), std::invalid_argument);
	std::swap(columns[0U], columns[1U]);
	EXPECT_THROW(static_cast<void>(Page::pack(columns, pageBytes, 3U)), std::invalid_argument);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, SharedPagePacksColumnsAndRejectsOverlappingDirectory) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	using Page = zyx::benchmark::PackedPropertyColumnPage;
	const std::vector<std::optional<PropertyValue>> first{PropertyValue(int64_t(9))}, second{PropertyValue("small")};
	const auto a = Column::encode(first, 1000U).encodeImage(1000U);
	const auto b = Column::encode(second, 1000U).encodeImage(1000U);
	const std::vector<Page::Column> input{{3U, a}, {9U, b}};
	const auto page = Page::encode(input, 4096U);
	EXPECT_EQ(page.size(), Page::HEADER + 2U * Page::ENTRY + a.size() + b.size());
	const auto decoded = Page::decode(page, 4096U);
	ASSERT_EQ(decoded.size(), 2U);
	EXPECT_EQ(decoded[1U].token, 9U);
	EXPECT_EQ(Column::decodeImage(decoded[0U].image, 1000U).read(0U), first[0U]);
	EXPECT_EQ(Column::decodeImage(decoded[1U].image, 1000U).read(0U), second[0U]);
	EXPECT_THROW(static_cast<void>(Page::encode(input, page.size() - 1U)), std::length_error);
	auto corrupt = page;
	LittleEndianCodec::writeU32(corrupt, Page::HEADER + Page::ENTRY + 4U, Page::HEADER + 2U * Page::ENTRY);
	LittleEndianCodec::writeU32(corrupt, 16U, 0U);
	LittleEndianCodec::writeU32(corrupt, 16U, graph::utils::calculateCrc(corrupt.data(), corrupt.size()));
	EXPECT_THROW(static_cast<void>(Page::decode(corrupt, 4096U)), std::runtime_error);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, BatchReadValidatesOnceAndChecksEachSlotForCancellation) {
	struct Checkpoint final : graph::storage::CancellationCheckpoint {
		mutable size_t calls = 0U;
		size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	std::vector<std::optional<PropertyValue>> values(64U);
	values[0U] = PropertyValue{};
	values[63U] = PropertyValue(int64_t(-8));
	const auto bytes = SparseFixedWidthPropertyColumn::encode(values);
	Checkpoint count;
	EXPECT_EQ(SparseFixedWidthPropertyColumn::readAll(bytes, &count), values);
	EXPECT_EQ(count.calls, 66U);
	for (size_t stop = 1U; stop <= count.calls; ++stop) {
		Checkpoint cancelled; cancelled.stop = stop;
		EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::readAll(bytes, &cancelled)), std::runtime_error);
		EXPECT_EQ(cancelled.calls, stop);
	}
	auto corrupt = bytes; corrupt.back() ^= std::byte{1U};
	Checkpoint invalid;
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::readAll(corrupt, &invalid)), std::runtime_error);
	EXPECT_EQ(invalid.calls, 1U);
	EXPECT_TRUE(SparseFixedWidthPropertyColumn::readAll(SparseFixedWidthPropertyColumn::encode({})).empty());
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, AdaptiveChecksumStreamsAndCancellationPreservesSource) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	struct Checkpoint final : graph::storage::CancellationCheckpoint {
		mutable size_t calls = 0U;
		size_t stop = 0U;
		void check() const override { if (++calls == stop) throw std::runtime_error("Test cancellation"); }
	};
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(std::string(150000U, 'x')), PropertyValue{}};
	const auto image = Column::encode(values, 200000U).encodeImage(200000U);
	const auto saved = image;
	Checkpoint count;
	EXPECT_EQ(Column::decodeImage(image, 200000U, &count).read(0U), values[0U]);
	EXPECT_GT(count.calls, 6U);
	for (size_t stop = 1U; stop <= count.calls; ++stop) {
		Checkpoint cancelled; cancelled.stop = stop;
		EXPECT_THROW(static_cast<void>(Column::decodeImage(image, 200000U, &cancelled)), std::runtime_error);
		EXPECT_EQ(cancelled.calls, stop);
		EXPECT_EQ(image, saved);
	}
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, AdaptiveImageRejectsChecksumValidStructuralCorruption) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	static_assert(!std::is_default_constructible_v<Column>);
	const std::vector<std::optional<PropertyValue>> values{std::nullopt, PropertyValue("text")};
	const auto original = Column::encode(values, 1000U).encodeImage(1000U);
	for (int mode = 0; mode < 9; ++mode) {
		SCOPED_TRACE(mode);
		auto image = original;
		if (mode == 0) LittleEndianCodec::writeU16(image, 4U, 2U);
		if (mode == 1) LittleEndianCodec::writeU16(image, 6U, 3U);
		if (mode == 2) LittleEndianCodec::writeU32(image, 28U, 1U);
		if (mode == 3) LittleEndianCodec::writeU64(image, 16U, 4U);
		if (mode == 4) LittleEndianCodec::writeU64(image, 16U, 1U);
		if (mode == 5) LittleEndianCodec::writeU32(image, Column::WIRE_HEADER, UINT32_MAX);
		if (mode == 6) { image.resize(Column::WIRE_HEADER + 2U); LittleEndianCodec::writeU32(image, 12U, 2U); }
		if (mode == 7) { image.push_back(std::byte{}); LittleEndianCodec::writeU32(image, 12U, static_cast<uint32_t>(image.size() - Column::WIRE_HEADER)); }
		if (mode == 8) LittleEndianCodec::writeU32(image, Column::WIRE_HEADER, 0U);
		LittleEndianCodec::writeU32(image, 24U, 0U);
		LittleEndianCodec::writeU32(image, 24U, graph::utils::calculateCrc(image.data(), image.size()));
		EXPECT_THROW(static_cast<void>(Column::decodeImage(image, 1000U)), std::runtime_error);
	}
	std::vector<std::optional<PropertyValue>> last(64U);
	last.back() = PropertyValue("last");
	const auto image = Column::encode(last, 1000U).encodeImage(1000U);
	const auto reopened = Column::decodeImage(image, 1000U);
	EXPECT_EQ(reopened.read(63U), last.back());
	EXPECT_FALSE(reopened.read(0U));
	const auto empty = Column::encode({}, 1000U).encodeImage(1000U);
	EXPECT_NO_THROW(static_cast<void>(Column::decodeImage(empty, 1000U)));
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, AdaptiveImageRoundTripRejectsCorruptionAndReconstructsTypeCounts) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	for (const auto &value : {PropertyValue(int64_t(7)), PropertyValue("text")}) {
		const std::vector<std::optional<PropertyValue>> values{std::nullopt, PropertyValue{}, value};
		const auto source = Column::encode(values, 1000U);
		const auto image = source.encodeImage(1000U);
		const auto reopened = Column::decodeImage(image, image.size());
		EXPECT_EQ(reopened.layout(), source.layout());
		EXPECT_EQ(reopened.encodeImage(image.size()), image);
		for (size_t row = 0U; row < values.size(); ++row) EXPECT_EQ(reopened.read(row), values[row]);
		EXPECT_EQ(reopened.replace(2U, std::nullopt, 1000U).layout(), Column::Layout::APC_FIXED);
		EXPECT_THROW(static_cast<void>(source.encodeImage(image.size() - 1U)), std::length_error);
		EXPECT_THROW(static_cast<void>(Column::decodeImage(image, image.size() - 1U)), std::runtime_error);
		auto corrupt = image; corrupt.back() ^= std::byte{1U};
		EXPECT_THROW(static_cast<void>(Column::decodeImage(corrupt, 1000U)), std::runtime_error);
		corrupt = image; LittleEndianCodec::writeU32(corrupt, 8U, 65U);
		LittleEndianCodec::writeU32(corrupt, 24U, 0U);
		LittleEndianCodec::writeU32(corrupt, 24U, graph::utils::calculateCrc(corrupt.data(), corrupt.size()));
		EXPECT_THROW(static_cast<void>(Column::decodeImage(corrupt, 1000U)), std::runtime_error);
	}
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, DecodedImageOwnsPayloadAfterSourceReleased) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(std::string(10000U, 'a')), PropertyValue(int64_t(7))};
	auto image = Column::encode(values, 20000U).encodeImage(20000U);
	const auto decoded = Column::decodeImage(image, 20000U);
	std::fill(image.begin(), image.end(), std::byte{});
	image.clear(); image.shrink_to_fit();
	EXPECT_EQ(decoded.read(0U), values[0U]);
	EXPECT_EQ(decoded.read(1U), values[1U]);
	const auto changed = decoded.replace(1U, PropertyValue(true), 20000U);
	EXPECT_EQ(changed.canonicalRoot(0U), decoded.canonicalRoot(0U));
	EXPECT_EQ(changed.read(1U), std::optional<PropertyValue>(PropertyValue(true)));
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, UnchangedUpdateReusesRootsButPreservesFloatingPointBits) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	const std::vector<std::optional<PropertyValue>> values{PropertyValue("same"), PropertyValue(-0.0), std::nullopt, PropertyValue{}};
	const auto before = Column::encode(values, 1000U);
	for (size_t row = 0U; row < values.size(); ++row) {
		const auto unchanged = before.replace(row, values[row], before.encodedBytes());
		for (size_t index = 0U; index < values.size(); ++index)
			EXPECT_EQ(unchanged.canonicalRoot(index), before.canonicalRoot(index));
	}
	EXPECT_THROW(static_cast<void>(before.replace(0U, values[0U], before.encodedBytes() - 1U)), std::length_error);
	EXPECT_THROW(static_cast<void>(before.replace(0U, values[0U], 0U)), std::invalid_argument);
	const auto changed = before.replace(1U, PropertyValue(0.0), 1000U);
	EXPECT_NE(changed.canonicalRoot(1U), before.canonicalRoot(1U));
	EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(changed.read(1U)->getVariant())), uint64_t{0U});
	EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(before.read(1U)->getVariant())), uint64_t{1U} << 63U);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, IncrementalTypeCountsHandleRepeatedTransitionsAndNullRemoval) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	std::vector<std::optional<PropertyValue>> expected{PropertyValue("a"), PropertyValue("b"), PropertyValue(int64_t(5)), std::nullopt};
	auto column = Column::encode(expected, 1000U);
	const std::vector<std::pair<size_t, std::optional<PropertyValue>>> updates{
		{0U, PropertyValue{}}, {1U, PropertyValue(true)}, {2U, std::nullopt},
		{3U, PropertyValue(2.0)}, {1U, std::nullopt}, {3U, PropertyValue("again")},
		{2U, PropertyValue(int64_t(6))}, {3U, std::nullopt}, {2U, PropertyValue{}}};
	for (const auto &[row, value] : updates) {
		expected[row] = value;
		column = column.replace(row, value, 1000U);
		EXPECT_EQ(column.layout() == Column::Layout::APC_FIXED,
		          SparseFixedWidthPropertyColumn::canEncode(expected));
		for (size_t index = 0U; index < expected.size(); ++index) EXPECT_EQ(column.read(index), expected[index]);
	}
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, CanonicalUpdateSharesUntouchedPayloadAndPreservesPinnedRoots) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	const std::vector<std::optional<PropertyValue>> values{PropertyValue("large"), PropertyValue{}, std::nullopt, PropertyValue(int64_t(3))};
	const auto before = Column::encode(values, 1000U);
	const auto pinned = before.canonicalRoot(0U);
	const auto changed = before.replace(3U, PropertyValue(true), 1000U);
	EXPECT_EQ(changed.canonicalRoot(0U), pinned);
	EXPECT_EQ(changed.canonicalRoot(1U), before.canonicalRoot(1U));
	EXPECT_NE(changed.canonicalRoot(3U), before.canonicalRoot(3U));
	const auto inserted = changed.replace(2U, PropertyValue(int64_t(4)), 1000U);
	EXPECT_EQ(inserted.canonicalRoot(3U), changed.canonicalRoot(3U));
	const auto removed = inserted.replace(1U, std::nullopt, 1000U);
	EXPECT_FALSE(removed.canonicalRoot(1U));
	EXPECT_EQ(removed.canonicalRoot(3U), changed.canonicalRoot(3U));
	EXPECT_THROW(static_cast<void>(removed.replace(3U, PropertyValue("too big"), 8U)), std::length_error);
	EXPECT_EQ(removed.read(3U), std::optional<PropertyValue>(PropertyValue(true)));
	EXPECT_EQ(before.read(3U), values[3U]);
	EXPECT_EQ(before.canonicalRoot(0U), pinned);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, CanonicalFallbackPacksPresentRowsAndAccountsForMask) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	std::vector<std::optional<PropertyValue>> values(64U);
	values[17U] = PropertyValue{};
	values[63U] = PropertyValue("last");
	size_t expected = sizeof(uint64_t);
	for (const size_t row : {17U, 63U}) {
		graph::storage::PropertyCollectionRoot root{row + 1U, 1U, {{1U, *values[row]}}};
		expected += graph::storage::EntityPropertyCollectionRootCodec::encode(root).size();
	}
	const auto column = Column::encode(values, expected);
	EXPECT_EQ(column.layout(), Column::Layout::APC_CANONICAL);
	EXPECT_EQ(column.encodedBytes(), expected);
	for (size_t row = 0U; row < values.size(); ++row) EXPECT_EQ(column.read(row), values[row]);
	EXPECT_THROW(static_cast<void>(Column::encode(values, expected - 1U)), std::length_error);
	const auto inserted = column.replace(0U, PropertyValue(true), expected + 100U);
	EXPECT_EQ(inserted.read(0U), std::optional<PropertyValue>(PropertyValue(true)));
	EXPECT_EQ(inserted.read(63U), values[63U]);
	EXPECT_EQ(column.read(0U), std::nullopt);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, AdaptiveFallbackSurvivesTypeChangesAndFailedReplacement) {
	using Column = zyx::benchmark::AdaptivePropertyColumn;
	const std::vector<std::optional<PropertyValue>> values{std::nullopt, PropertyValue{}, PropertyValue(int64_t(7))};
	const auto fixed = Column::encode(values, 20000U);
	EXPECT_EQ(fixed.layout(), Column::Layout::APC_FIXED);
	const auto mixed = fixed.replace(0U, PropertyValue(std::string(16000U, 'x')), 20000U);
	EXPECT_EQ(mixed.layout(), Column::Layout::APC_CANONICAL);
	EXPECT_EQ(mixed.read(0U), std::optional<PropertyValue>(PropertyValue(std::string(16000U, 'x'))));
	EXPECT_EQ(mixed.read(1U), values[1U]);
	EXPECT_EQ(mixed.read(2U), values[2U]);
	EXPECT_THROW(static_cast<void>(mixed.replace(2U, PropertyValue(true), 10U)), std::length_error);
	EXPECT_EQ(mixed.read(2U), values[2U]);
	const auto restored = mixed.replace(0U, std::nullopt, 20000U);
	EXPECT_EQ(restored.layout(), Column::Layout::APC_FIXED);
	for (size_t row = 0U; row < values.size(); ++row) {
		EXPECT_EQ(fixed.read(row), values[row]);
		EXPECT_EQ(restored.read(row), values[row]);
	}
	EXPECT_THROW(static_cast<void>(restored.read(3U)), std::out_of_range);
	EXPECT_THROW(static_cast<void>(Column::encode(values, 0U)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(Column::encode(values, 1U)), std::length_error);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, PacksOnlyValidValuesAndPreservesMissingNullAndLastRow) {
	std::vector<std::optional<PropertyValue>> values(64U);
	values[0U] = PropertyValue{};
	values[17U] = PropertyValue(int64_t(-7));
	values[63U] = PropertyValue(int64_t(99));
	const auto bytes = SparseFixedWidthPropertyColumn::encode(values);
	EXPECT_EQ(bytes.size(), SparseFixedWidthPropertyColumn::HEADER_BYTES + 16U);
	for (size_t row = 0U; row < values.size(); ++row) EXPECT_EQ(SparseFixedWidthPropertyColumn::read(bytes, row), values[row]);
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::read(bytes, 64U)), std::out_of_range);
	RecordProperty("sparse_column_bytes", static_cast<int>(bytes.size()));
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, RoundTripsBooleanDoubleAndNullOnlyColumnsWithoutCoercion) {
	for (const auto &value : {PropertyValue(true), PropertyValue(false), PropertyValue(-0.0), PropertyValue(1.25)}) {
		const std::vector<std::optional<PropertyValue>> values{std::nullopt, PropertyValue{}, value};
		const auto bytes = SparseFixedWidthPropertyColumn::encode(values);
		const auto result = SparseFixedWidthPropertyColumn::read(bytes, 2U);
		ASSERT_TRUE(result.has_value());
		EXPECT_EQ(result->getVariant().index(), value.getVariant().index());
		EXPECT_EQ(*result, value);
		if (std::holds_alternative<double>(value.getVariant()))
			EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(result->getVariant())), std::bit_cast<uint64_t>(std::get<double>(value.getVariant())));
	}
	const std::vector<std::optional<PropertyValue>> nulls{std::nullopt, PropertyValue{}};
	const auto bytes = SparseFixedWidthPropertyColumn::encode(nulls);
	EXPECT_EQ(bytes.size(), SparseFixedWidthPropertyColumn::HEADER_BYTES);
	EXPECT_FALSE(SparseFixedWidthPropertyColumn::read(bytes, 0U).has_value());
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(bytes, 1U), std::optional<PropertyValue>(PropertyValue{}));
	EXPECT_NO_THROW(SparseFixedWidthPropertyColumn::validate(SparseFixedWidthPropertyColumn::encode({})));
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, RejectsUnsupportedInputsAndChecksumValidMalformedShapes) {
	const std::vector<std::optional<PropertyValue>> mixed{PropertyValue(int64_t(1)), PropertyValue(1.0)};
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::encode(mixed)), std::invalid_argument);
	const std::vector<std::optional<PropertyValue>> strings{PropertyValue("text")};
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::encode(strings)), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::encode(std::vector<std::optional<PropertyValue>>(65U))), std::invalid_argument);
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(true)};
	const auto original = SparseFixedWidthPropertyColumn::encode(values);
	for (int mode = 0; mode < 8; ++mode) {
		SCOPED_TRACE(mode);
		auto bytes = original;
		if (mode == 0) LittleEndianCodec::writeU16(bytes, 4U, 2U);
		if (mode == 1) LittleEndianCodec::writeU16(bytes, 6U, 5U);
		if (mode == 2) LittleEndianCodec::writeU32(bytes, 8U, 65U);
		if (mode == 3) LittleEndianCodec::writeU64(bytes, 16U, 2U);
		if (mode == 4) LittleEndianCodec::writeU64(bytes, 24U, 2U);
		if (mode == 5) LittleEndianCodec::writeU64(bytes, 32U, 1U);
		if (mode == 6) LittleEndianCodec::writeU32(bytes, 12U, 2U);
		if (mode == 7) bytes.back() = std::byte{2U};
		LittleEndianCodec::writeU32(bytes, 40U, 0U);
		LittleEndianCodec::writeU32(bytes, 40U, graph::utils::calculateCrc(bytes.data(), bytes.size()));
		EXPECT_THROW(SparseFixedWidthPropertyColumn::validate(bytes), std::runtime_error);
	}
	auto corrupted = original; corrupted.back() ^= std::byte{1U};
	EXPECT_THROW(SparseFixedWidthPropertyColumn::validate(corrupted), std::runtime_error);
	auto truncated = original; truncated.resize(47U);
	EXPECT_THROW(SparseFixedWidthPropertyColumn::validate(truncated), std::runtime_error);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, ImmutableUpdateMaintainsRankAndDistinguishesRemovalFromNull) {
	const std::vector<std::optional<PropertyValue>> values{
		PropertyValue(int64_t(1)), std::nullopt, PropertyValue(int64_t(3)), PropertyValue{}};
	const auto before = SparseFixedWidthPropertyColumn::encode(values);
	const auto inserted = SparseFixedWidthPropertyColumn::replace(before, 1U, PropertyValue(int64_t(2)));
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(inserted, 2U), values[2U]);
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(before, 1U), std::nullopt);
	const auto removed = SparseFixedWidthPropertyColumn::replace(inserted, 0U, std::nullopt);
	EXPECT_FALSE(SparseFixedWidthPropertyColumn::read(removed, 0U).has_value());
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(removed, 1U), std::optional<PropertyValue>(PropertyValue(int64_t(2))));
	const auto nulled = SparseFixedWidthPropertyColumn::replace(removed, 1U, PropertyValue{});
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(nulled, 1U), std::optional<PropertyValue>(PropertyValue{}));
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(nulled, 2U), values[2U]);
	const auto onlyNulls = SparseFixedWidthPropertyColumn::replace(nulled, 2U, std::nullopt);
	EXPECT_EQ(onlyNulls.size(), SparseFixedWidthPropertyColumn::HEADER_BYTES);
	const auto changedType = SparseFixedWidthPropertyColumn::replace(onlyNulls, 0U, PropertyValue(true));
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(changedType, 0U), std::optional<PropertyValue>(PropertyValue(true)));
	for (size_t row = 0U; row < values.size(); ++row) EXPECT_EQ(SparseFixedWidthPropertyColumn::read(before, row), values[row]);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, FailedUpdatePreservesSourceAndRejectsCorruptInput) {
	const std::vector<std::optional<PropertyValue>> values{PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	const auto before = SparseFixedWidthPropertyColumn::encode(values);
	const auto saved = before;
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::replace(before, 0U, PropertyValue(true))), std::invalid_argument);
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::replace(before, 2U, std::nullopt)), std::out_of_range);
	EXPECT_EQ(before, saved);
	EXPECT_EQ(SparseFixedWidthPropertyColumn::read(before, 1U), values[1U]);
	auto corrupted = before;
	corrupted.back() ^= std::byte{1U};
	EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::replace(corrupted, 0U, std::nullopt)), std::runtime_error);
}

TEST(SparseFixedWidthPropertyColumnPrototypeTest, LayoutEligibilityPreservesUnsupportedValuesForCanonicalFallback) {
	std::vector<std::optional<PropertyValue>> values{std::nullopt, PropertyValue{}, PropertyValue(int64_t(1)), PropertyValue(int64_t(2))};
	EXPECT_TRUE(SparseFixedWidthPropertyColumn::canEncode(values));
	EXPECT_NO_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::encode(values)));
	for (const auto &replacement : {PropertyValue(true), PropertyValue(2.0), PropertyValue(std::string(16000U, 'v'))}) {
		values.back() = replacement;
		EXPECT_FALSE(SparseFixedWidthPropertyColumn::canEncode(values));
		EXPECT_THROW(static_cast<void>(SparseFixedWidthPropertyColumn::encode(values)), std::invalid_argument);
		for (size_t row = 0U; row < values.size(); ++row) {
			graph::storage::PropertyCollectionRoot root{row + 1U, 1U, {}};
			if (values[row]) root.entries.push_back({7U, *values[row]});
			const auto encoded = graph::storage::EntityPropertyCollectionRootCodec::encode(root);
			EXPECT_EQ(graph::storage::EntityPropertyCollectionRootCodec::decode(encoded), root);
		}
	}
	values.back() = PropertyValue(int64_t(2));
	EXPECT_TRUE(SparseFixedWidthPropertyColumn::canEncode(values));
	EXPECT_FALSE(SparseFixedWidthPropertyColumn::canEncode(std::vector<std::optional<PropertyValue>>(65U)));
}
