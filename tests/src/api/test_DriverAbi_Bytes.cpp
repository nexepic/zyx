/**
 * @file test_DriverAbi_Bytes.cpp
 * @brief Driver ABI binary Value input, output, and nested reference tests.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "zyx/zyx_driver_abi.h"

namespace {

namespace fs = std::filesystem;

std::string uniqueDbPath() {
	static std::atomic<uint64_t> sequence{0U};
	const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	return (fs::temp_directory_path() / ("zyx_driver_abi_bytes_" + std::to_string(now) + "_" +
											std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed))))
				.string();
}

} // namespace

class DriverAbiBytesTest : public ::testing::Test {
protected:
	std::string path;
	zyx_driver_db_t *db = nullptr;
	zyx_driver_error_t *error = nullptr;

	void SetUp() override {
		path = uniqueDbPath();
		std::error_code ec;
		fs::remove(path, ec);
		ASSERT_EQ(zyx_driver_db_open(path.c_str(), &db, &error), ZYX_DRIVER_OK);
		ASSERT_EQ(error, nullptr);
	}

	void TearDown() override {
		if (db != nullptr) {
			EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
			db = nullptr;
		}
		zyx_driver_error_free(error);
		error = nullptr;
		std::error_code ec;
		fs::remove(path, ec);
	}
};

TEST_F(DriverAbiBytesTest, RoundTripsTopLevelAndEmptyBinaryValues) {
	const std::vector<uint8_t> expected{0U, 1U, 0x7FU, 0x80U, 0xFFU};
	zyx_driver_params_t *params = nullptr;
	zyx_driver_result_t *result = nullptr;

	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", expected.data(), static_cast<uint32_t>(expected.size()),
											 &error),
				 ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $payload AS payload", params, &result, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	EXPECT_EQ(zyx_driver_result_value_type(result, 0), ZYX_DRIVER_VALUE_BYTES);

	const uint8_t *actual = nullptr;
	uint32_t actualSize = 0U;
	ASSERT_EQ(zyx_driver_result_get_bytes(result, 0, &actual, &actualSize, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(actualSize, expected.size());
	ASSERT_NE(actual, nullptr);
	EXPECT_EQ(std::vector<uint8_t>(actual, actual + actualSize), expected);

	zyx_driver_value_ref_t ref{};
	ASSERT_EQ(zyx_driver_result_get_value(result, 0, &ref, &error), ZYX_DRIVER_OK);
	actual = nullptr;
	actualSize = 0U;
	ASSERT_EQ(zyx_driver_value_ref_get_bytes(&ref, &actual, &actualSize, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(std::vector<uint8_t>(actual, actual + actualSize), expected);

	zyx_driver_value_t *empty = nullptr;
	ASSERT_EQ(zyx_driver_value_bytes_create(nullptr, 0U, &empty, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_value(params, "empty", empty, &error), ZYX_DRIVER_OK);
	zyx_driver_value_free(empty, &error);
	result = nullptr;
	ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $empty AS empty", params, &result, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	actual = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(1U));
	actualSize = 99U;
	ASSERT_EQ(zyx_driver_result_get_bytes(result, 0, &actual, &actualSize, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(actual, nullptr);
	EXPECT_EQ(actualSize, 0U);

	zyx_driver_result_free(result);
	zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiBytesTest, StreamsBoundedRangesFromResultsAndReferences) {
	const std::vector<uint8_t> expected{0U, 1U, 2U, 3U, 4U, 5U};
	zyx_driver_params_t *params = nullptr;
	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", expected.data(), expected.size(), &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $payload AS payload", params, &result, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

	zyx_driver_bytes_stream_t *stream = nullptr;
	ASSERT_EQ(zyx_driver_result_open_bytes_stream(result, 0U, 1U, 4U, &stream, &error), ZYX_DRIVER_OK);
	ASSERT_NE(stream, nullptr);
	std::vector<uint8_t> actual;
	std::array<uint8_t, 2> buffer{};
	uint32_t read = 0U;
	EXPECT_EQ(zyx_driver_bytes_stream_read(stream, buffer.data(), buffer.size(), &read, &error), ZYX_DRIVER_ROW);
	actual.insert(actual.end(), buffer.begin(), buffer.begin() + read);
	EXPECT_EQ(zyx_driver_bytes_stream_read(stream, buffer.data(), buffer.size(), &read, &error), ZYX_DRIVER_ROW);
	actual.insert(actual.end(), buffer.begin(), buffer.begin() + read);
	EXPECT_EQ(zyx_driver_bytes_stream_read(stream, buffer.data(), buffer.size(), &read, &error), ZYX_DRIVER_DONE);
	EXPECT_EQ(actual, (std::vector<uint8_t>{1U, 2U, 3U, 4U}));
	zyx_driver_bytes_stream_close(stream, &error);

	zyx_driver_value_ref_t ref{};
	ASSERT_EQ(zyx_driver_result_get_value(result, 0U, &ref, &error), ZYX_DRIVER_OK);
	stream = nullptr;
	ASSERT_EQ(zyx_driver_value_ref_open_bytes_stream(&ref, 2U, 2U, &stream, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_bytes_stream_read(stream, buffer.data(), buffer.size(), &read, &error), ZYX_DRIVER_ROW);
	EXPECT_EQ(std::vector<uint8_t>(buffer.begin(), buffer.begin() + read), (std::vector<uint8_t>{2U, 3U}));
	EXPECT_EQ(zyx_driver_bytes_stream_read(stream, buffer.data(), buffer.size(), &read, &error), ZYX_DRIVER_DONE);
	zyx_driver_bytes_stream_close(stream, &error);

	zyx_driver_result_free(result);
	zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiBytesTest, RejectsInvalidByteStreamRangesAndBuffers) {
	const std::vector<uint8_t> expected{1U, 2U, 3U};
	zyx_driver_params_t *params = nullptr;
	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", expected.data(), expected.size(), &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $payload", params, &result, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	zyx_driver_bytes_stream_t *stream = nullptr;
	EXPECT_EQ(zyx_driver_result_open_bytes_stream(result, 0U, 4U, 0U, &stream, &error), ZYX_DRIVER_OUT_OF_RANGE);
	zyx_driver_error_free(error);
	error = nullptr;
	EXPECT_EQ(zyx_driver_result_open_bytes_stream(result, 0U, 2U, 2U, &stream, &error), ZYX_DRIVER_OUT_OF_RANGE);
	zyx_driver_error_free(error);
	error = nullptr;
	ASSERT_EQ(zyx_driver_result_open_bytes_stream(result, 0U, 0U, expected.size(), &stream, &error), ZYX_DRIVER_OK);
	uint32_t read = 0U;
	EXPECT_EQ(zyx_driver_bytes_stream_read(stream, nullptr, 1U, &read, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	zyx_driver_bytes_stream_close(stream, &error);
	zyx_driver_result_free(result);
	zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiBytesTest, RoundTripsBytesInsideListAndMapValueReferences) {
	const std::vector<uint8_t> listBytes{0x10U, 0x00U};
	const std::vector<uint8_t> mapBytes{0xFEU, 0xFFU};
	zyx_driver_params_t *params = nullptr;
	zyx_driver_value_t *list = nullptr;
	zyx_driver_value_t *map = nullptr;
	zyx_driver_result_t *result = nullptr;

	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_value_list_create(&list, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_value_list_append_bytes(list, listBytes.data(), listBytes.size(), &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_value_map_create(&map, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_value_map_set_bytes(map, "payload", mapBytes.data(), mapBytes.size(), &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_value(params, "list", list, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_value(params, "map", map, &error), ZYX_DRIVER_OK);

	ASSERT_EQ(zyx_driver_db_execute(db, "RETURN $list AS list, $map AS map", params, &result, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

	zyx_driver_value_ref_t listRef{};
	zyx_driver_value_ref_t mapRef{};
	ASSERT_EQ(zyx_driver_result_get_value(result, 0, &listRef, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_get_value(result, 1, &mapRef, &error), ZYX_DRIVER_OK);
	uint32_t listCount = 0U;
	ASSERT_EQ(zyx_driver_value_ref_list_count(&listRef, &listCount, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(listCount, 1U);
	const uint8_t *actual = nullptr;
	uint32_t actualSize = 0U;
	ASSERT_EQ(zyx_driver_value_ref_list_get_bytes(&listRef, 0U, &actual, &actualSize, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(std::vector<uint8_t>(actual, actual + actualSize), listBytes);
	ASSERT_EQ(zyx_driver_value_ref_map_get_bytes(&mapRef, "payload", &actual, &actualSize, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(std::vector<uint8_t>(actual, actual + actualSize), mapBytes);

	zyx_driver_result_free(result);
	zyx_driver_value_free(map, &error);
	zyx_driver_value_free(list, &error);
	zyx_driver_params_free(params, &error);
}

TEST_F(DriverAbiBytesTest, StreamsPersistedLargeValueRangesAfterReopenAndResultRelease) {
	// This deliberately exceeds one slotted Property-page record, so the native
	// Property store must materialize and later resolve a LargeValue reference.
	std::vector<uint8_t> expected(16U * 1024U + 137U);
	for (size_t index = 0U; index < expected.size(); ++index) {
		expected[index] = static_cast<uint8_t>((index * 37U) % 251U);
	}

	zyx_driver_params_t *params = nullptr;
	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", expected.data(),
									 static_cast<uint32_t>(expected.size()), &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_execute(db, "CREATE (:PersistedBinary {payload: $payload})", params, &result, &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	zyx_driver_params_free(params, &error);
	params = nullptr;

	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;
	EXPECT_FALSE(fs::exists(path + ".wal"));
	ASSERT_EQ(zyx_driver_db_open(path.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:PersistedBinary) RETURN n.payload", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);

	constexpr uint32_t offset = 31U;
	const uint32_t length = static_cast<uint32_t>(expected.size() - 73U);
	zyx_driver_bytes_stream_t *stream = nullptr;
	ASSERT_EQ(zyx_driver_result_open_bytes_stream(result, 0U, offset, length, &stream, &error), ZYX_DRIVER_OK);
	ASSERT_NE(stream, nullptr);

	// A pull stream owns an immutable range snapshot, rather than borrowing the
	// result cursor that created it. This is essential for a persisted LargeValue.
	zyx_driver_result_free(result);
	result = nullptr;

	std::array<uint8_t, 997U> buffer{};
	std::vector<uint8_t> actual;
	actual.reserve(length);
	for (;;) {
		uint32_t read = 0U;
		const zyx_driver_status_t status =
				zyx_driver_bytes_stream_read(stream, buffer.data(), static_cast<uint32_t>(buffer.size()), &read, &error);
		if (status == ZYX_DRIVER_DONE) {
			EXPECT_EQ(read, 0U);
			break;
		}
		ASSERT_EQ(status, ZYX_DRIVER_ROW);
		ASSERT_GT(read, 0U);
		actual.insert(actual.end(), buffer.begin(), buffer.begin() + read);
	}
	EXPECT_EQ(actual, (std::vector<uint8_t>(expected.begin() + offset, expected.begin() + offset + length)));
	zyx_driver_bytes_stream_close(stream, &error);
	EXPECT_EQ(error, nullptr);
}

TEST_F(DriverAbiBytesTest, ExplicitTransactionPersistsLargeValueReplacementAndRemovalAcrossReopen) {
	std::vector<uint8_t> committed(16U * 1024U + 137U);
	std::vector<uint8_t> replacement(committed.size());
	std::vector<uint8_t> discarded(committed.size());
	for (size_t index = 0U; index < committed.size(); ++index) {
		committed[index] = static_cast<uint8_t>((index * 17U) % 251U);
		replacement[index] = static_cast<uint8_t>((index * 31U + 7U) % 251U);
		discarded[index] = static_cast<uint8_t>((index * 43U + 1U) % 251U);
	}

	zyx_driver_txn_t *transaction = nullptr;
	zyx_driver_params_t *params = nullptr;
	zyx_driver_result_t *result = nullptr;
	ASSERT_EQ(zyx_driver_txn_begin(db, &transaction, &error), ZYX_DRIVER_OK);
	ASSERT_NE(transaction, nullptr);
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", committed.data(), static_cast<uint32_t>(committed.size()),
										 &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "CREATE (:TransactionBinary {payload: $payload})", params, &result,
										 &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	zyx_driver_params_free(params, &error);
	params = nullptr;
	ASSERT_EQ(zyx_driver_txn_commit(transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_close(transaction, &error), ZYX_DRIVER_OK);
	transaction = nullptr;

	// A committed replacement must survive independently from a later removal.
	ASSERT_EQ(zyx_driver_txn_begin(db, &transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", replacement.data(),
									 static_cast<uint32_t>(replacement.size()), &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "MATCH (n:TransactionBinary) SET n.payload = $payload", params,
									 &result, &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	zyx_driver_params_free(params, &error);
	params = nullptr;
	ASSERT_EQ(zyx_driver_txn_commit(transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_close(transaction, &error), ZYX_DRIVER_OK);
	transaction = nullptr;

	ASSERT_EQ(zyx_driver_txn_begin(db, &transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", committed.data(), static_cast<uint32_t>(committed.size()),
									 &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "CREATE (:TransactionBinaryRemoved {payload: $payload})", params,
									 &result, &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	zyx_driver_params_free(params, &error);
	params = nullptr;
	ASSERT_EQ(zyx_driver_txn_commit(transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_close(transaction, &error), ZYX_DRIVER_OK);
	transaction = nullptr;

	ASSERT_EQ(zyx_driver_txn_begin(db, &transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_params_set_bytes(params, "payload", discarded.data(), static_cast<uint32_t>(discarded.size()),
										 &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "MATCH (n:TransactionBinary) SET n.payload = $payload", params,
										 &result, &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	zyx_driver_params_free(params, &error);
	params = nullptr;
	ASSERT_EQ(zyx_driver_txn_rollback(transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_close(transaction, &error), ZYX_DRIVER_OK);
	transaction = nullptr;

	ASSERT_EQ(zyx_driver_txn_begin(db, &transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_execute(transaction, "MATCH (n:TransactionBinaryRemoved) REMOVE n.payload", nullptr,
									 &result, &error),
			  ZYX_DRIVER_OK);
	zyx_driver_result_free(result);
	result = nullptr;
	ASSERT_EQ(zyx_driver_txn_commit(transaction, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_txn_close(transaction, &error), ZYX_DRIVER_OK);
	transaction = nullptr;

	ASSERT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
	db = nullptr;
	ASSERT_EQ(zyx_driver_db_open(path.c_str(), &db, &error), ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:TransactionBinary) RETURN n.payload", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	const uint8_t *actual = nullptr;
	uint32_t actualSize = 0U;
	ASSERT_EQ(zyx_driver_result_get_bytes(result, 0U, &actual, &actualSize, &error), ZYX_DRIVER_OK);
	ASSERT_NE(actual, nullptr);
	EXPECT_EQ(actualSize, replacement.size());
	EXPECT_EQ(std::vector<uint8_t>(actual, actual + actualSize), replacement);
	zyx_driver_result_free(result);
	result = nullptr;

	ASSERT_EQ(zyx_driver_db_execute(db, "MATCH (n:TransactionBinaryRemoved) RETURN n.payload", nullptr, &result, &error),
			  ZYX_DRIVER_OK);
	ASSERT_EQ(zyx_driver_result_next(result, &error), ZYX_DRIVER_ROW);
	EXPECT_EQ(zyx_driver_result_value_type(result, 0U), ZYX_DRIVER_VALUE_NULL);
	zyx_driver_result_free(result);
}

TEST_F(DriverAbiBytesTest, RejectsNullNonEmptyInputAndInvalidByteOutputs) {
	zyx_driver_params_t *params = nullptr;
	zyx_driver_value_t *value = nullptr;
	ASSERT_EQ(zyx_driver_params_create(&params, &error), ZYX_DRIVER_OK);

	EXPECT_EQ(zyx_driver_params_set_bytes(params, "bad", nullptr, 1U, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	zyx_driver_error_free(error);
	error = nullptr;
	EXPECT_EQ(zyx_driver_value_bytes_create(nullptr, 1U, &value, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	zyx_driver_error_free(error);
	error = nullptr;
	EXPECT_EQ(zyx_driver_value_bytes_create(nullptr, 0U, nullptr, &error), ZYX_DRIVER_INVALID_ARGUMENT);
	zyx_driver_error_free(error);
	error = nullptr;

	zyx_driver_params_free(params, &error);
}
