#include <array>
#include <stdexcept>
#include <vector>
#include <gtest/gtest.h>
#include "storage/relocatable/property/PropertyBatchOrder.hpp"

using namespace graph::storage;

TEST(PropertyBatchOrderTest, RestoresSparseOwnerOrdinalsByPhysicalPageAndSlot) {
	const std::array<std::optional<EntityPropertyLocation>, 4U> locations{
		EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, 1U}, PageId(30U), 1U, 1U},
		std::nullopt,
		EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, 3U}, PageId(10U), 7U, 1U},
		EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, 4U}, PageId(10U), 2U, 1U}};
	EXPECT_EQ(detail::propertyPageOrder(locations, nullptr), (std::vector<size_t>{3U, 2U, 0U}));
	const std::array<std::optional<EntityPropertyLocation>, 4U> absent{};
	const auto empty = detail::propertyPageOrder(absent, nullptr);
	EXPECT_TRUE(empty.empty());
	EXPECT_EQ(empty.capacity(), 0U);
}

TEST(PropertyBatchOrderTest, CancellationInterruptsCpuWorkAndLeavesLocationsReusable) {
	std::vector<std::optional<EntityPropertyLocation>> locations;
	for (size_t index = 0U; index < 1000U; ++index)
		locations.push_back(EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, index + 1U},
			PageId((index * 17U) % 29U + 1U), static_cast<uint16_t>(index % 5U), 1U});
	const auto original = locations;
	class Budget final : public CancellationCheckpoint {
	public:
		explicit Budget(size_t checks) : remaining(checks) {}
		mutable size_t remaining;
		void check() const override {
			if (remaining == 0U) throw std::runtime_error("CPU work cancelled");
			--remaining;
		}
	};
	for (const size_t allowed: {0U, 3U, 7U, 12U}) {
		Budget cancellation(allowed);
		EXPECT_THROW(static_cast<void>(detail::propertyPageOrder(locations, &cancellation)), std::runtime_error);
		EXPECT_EQ(locations, original);
	}
	const auto retried = detail::propertyPageOrder(locations, nullptr);
	ASSERT_EQ(retried.size(), locations.size());
	for (size_t index = 1U; index < retried.size(); ++index) {
		const auto &previous = *locations[retried[index - 1U]];
		const auto &current = *locations[retried[index]];
		EXPECT_LE(previous.page_id.value(), current.page_id.value());
		if (previous.page_id == current.page_id) EXPECT_LE(previous.slot_id, current.slot_id);
	}
}

TEST(PropertyBatchOrderTest, AdmitsSparseOrderBeforeReserveAndRetriesWithoutChangingLocations) {
	const std::array<std::optional<EntityPropertyLocation>, 3U> locations{
		EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, 1U}, PageId(30U), 1U, 1U},
		std::nullopt,
		EntityPropertyLocation{{EntityPropertyOwnerKind::EPOK_NODE, 3U}, PageId(10U), 7U, 1U}};
	size_t requested = 0U;
	const SelectedPropertyReadAdmission reject = [&](size_t bytes) { requested = bytes; throw std::length_error("order slots rejected"); };
	EXPECT_THROW(static_cast<void>(detail::propertyPageOrder(locations, nullptr, &reject)), std::length_error);
	EXPECT_EQ(requested, 2U * sizeof(size_t));
	for (int retry = 0; retry < 2; ++retry) {
		const SelectedPropertyReadAdmission accept = [&](size_t bytes) { requested = bytes; };
		const auto result = detail::propertyPageOrder(locations, nullptr, &accept);
		EXPECT_EQ(result, (std::vector<size_t>{2U, 0U}));
		EXPECT_EQ(requested, result.capacity() * sizeof(size_t));
	}
	EXPECT_EQ(locations[0]->page_id, PageId(30U));
	EXPECT_FALSE(locations[1]);
}
