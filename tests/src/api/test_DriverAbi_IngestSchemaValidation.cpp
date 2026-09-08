/**
 * @file test_DriverAbi_IngestSchemaValidation.cpp
 * @brief Arrow schema validation contracts for the Driver ABI ingestor.
 */

#include <array>
#include <chrono>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "zyx/zyx_driver_abi.h"

namespace {

namespace fs = std::filesystem;

class DriverAbiIngestSchemaValidationTest : public ::testing::Test {
protected:
	void SetUp() override {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		dbPath = (fs::temp_directory_path() / ("zyx_driver_abi_schema_" + std::to_string(stamp))).string();
		zyx_driver_error_t *error = nullptr;
		ASSERT_EQ(zyx_driver_db_open(dbPath.c_str(), &db, &error), ZYX_DRIVER_OK);
		ASSERT_EQ(error, nullptr);
	}

	void TearDown() override {
		if (db != nullptr) {
			zyx_driver_error_t *error = nullptr;
			EXPECT_EQ(zyx_driver_db_close(db, &error), ZYX_DRIVER_OK);
			if (error != nullptr) {
				zyx_driver_error_free(error);
			}
			db = nullptr;
		}
		std::error_code ec;
		fs::remove_all(dbPath, ec);
	}

	void expectPrepareRejects(const ArrowSchema *schema) {
		zyx_driver_error_t *error = nullptr;
		zyx_driver_ingest_t *ingest = nullptr;
		ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
		ASSERT_NE(ingest, nullptr);
		ASSERT_EQ(error, nullptr);

		zyx_driver_edge_ingestor_t *ingestor = nullptr;
		const zyx_driver_status_t status =
				zyx_driver_ingest_prepare_edges(ingest, "SCHEMA_REL", schema, &ingestor, &error);
		EXPECT_NE(status, ZYX_DRIVER_OK);
		EXPECT_EQ(ingestor, nullptr);
		ASSERT_NE(error, nullptr);
		zyx_driver_error_free(error);
		error = nullptr;
		EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(error, nullptr);
	}

	std::string dbPath;
	zyx_driver_db_t *db = nullptr;
};

ArrowSchema scalar(const char *format, const char *name = "value") {
	return ArrowSchema{format, name, nullptr, ARROW_FLAG_NULLABLE, 0, nullptr, nullptr, nullptr, nullptr};
}

} // namespace

TEST_F(DriverAbiIngestSchemaValidationTest, RejectsNullAndMalformedTopLevelSchemas) {
	expectPrepareRejects(nullptr);

	ArrowSchema nullFormat = scalar(nullptr, "source_id");
	expectPrepareRejects(&nullFormat);

	ArrowSchema negativeChildren = scalar("+s", "edges");
	negativeChildren.n_children = -1;
	expectPrepareRejects(&negativeChildren);

	ArrowSchema tooManyChildren = scalar("+s", "edges");
	tooManyChildren.n_children = 1025;
	expectPrepareRejects(&tooManyChildren);

	ArrowSchema unsupported = scalar("not-an-arrow-format", "edges");
	expectPrepareRejects(&unsupported);
}

TEST_F(DriverAbiIngestSchemaValidationTest, RejectsInvalidNestedAndDictionarySchemas) {
	ArrowSchema source = scalar("l", "source_id");
	ArrowSchema target = scalar("l", "target_id");
	ArrowSchema child = scalar("l", "entry");
	ArrowSchema *childPointer = &child;
	ArrowSchema list = {"+l", "properties", nullptr, ARROW_FLAG_NULLABLE, 1, &childPointer, nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 3> fields{&source, &target, &list};
	ArrowSchema root{"+s", "edges", nullptr, 0, 3, fields.data(), nullptr, nullptr, nullptr};
	// Edge properties must be a struct; a list exercises the one-child shape guard.
	expectPrepareRejects(&root);

	ArrowSchema primitiveWithChild = scalar("l", "properties");
	primitiveWithChild.n_children = 1;
	primitiveWithChild.children = &childPointer;
	fields[2] = &primitiveWithChild;
	expectPrepareRejects(&root);

	ArrowSchema nullChildren = {"+s", "properties", nullptr, ARROW_FLAG_NULLABLE, 1, nullptr, nullptr, nullptr, nullptr};
	fields[2] = &nullChildren;
	expectPrepareRejects(&root);

	ArrowSchema dictionaryIndex = scalar("u", "properties");
	ArrowSchema dictionaryValues = scalar("u", "dictionary");
	dictionaryIndex.dictionary = &dictionaryValues;
	fields[2] = &dictionaryIndex;
	expectPrepareRejects(&root);

	ArrowSchema dictionaryWithChildren = scalar("l", "properties");
	dictionaryWithChildren.dictionary = &dictionaryValues;
	dictionaryWithChildren.n_children = 1;
	dictionaryWithChildren.children = &childPointer;
	fields[2] = &dictionaryWithChildren;
	expectPrepareRejects(&root);
}

TEST_F(DriverAbiIngestSchemaValidationTest, RejectsFixedSizeListAndStructNameViolations) {
	ArrowSchema source = scalar("l", "source_id");
	ArrowSchema target = scalar("l", "target_id");
	ArrowSchema fixed = {"+w:0", "properties", nullptr, ARROW_FLAG_NULLABLE, 0, nullptr, nullptr, nullptr, nullptr};
	std::array<ArrowSchema *, 3> fields{&source, &target, &fixed};
	ArrowSchema root{"+s", "edges", nullptr, 0, 3, fields.data(), nullptr, nullptr, nullptr};
	expectPrepareRejects(&root);

	fixed.format = "+w:bad";
	expectPrepareRejects(&root);

	fixed.format = "+w:2";
	fixed.n_children = 0;
	expectPrepareRejects(&root);

	ArrowSchema emptyName = scalar("l", nullptr);
	ArrowSchema *emptyNamePointer = &emptyName;
	ArrowSchema properties = {"+s", "properties", nullptr, ARROW_FLAG_NULLABLE, 1, &emptyNamePointer, nullptr, nullptr, nullptr};
	fields[2] = &properties;
	expectPrepareRejects(&root);

	ArrowSchema duplicateA = scalar("l", "same");
	ArrowSchema duplicateB = scalar("u", "same");
	std::array<ArrowSchema *, 2> duplicatePointers{&duplicateA, &duplicateB};
	properties.n_children = 2;
	properties.children = duplicatePointers.data();
	expectPrepareRejects(&root);
}

TEST_F(DriverAbiIngestSchemaValidationTest, AcceptsAllPrimitiveArrowPropertyKinds) {
	const std::array<const char *, 14> formats{
			"n", "b", "c", "C", "s", "S", "i", "I", "l", "L", "f", "g", "u", "U"};
	for (const char *format : formats) {
		ArrowSchema source = scalar("l", "source_id");
		ArrowSchema target = scalar("l", "target_id");
		ArrowSchema value = scalar(format, "value");
		ArrowSchema *valuePointer = &value;
		ArrowSchema properties = {"+s", "properties", nullptr, ARROW_FLAG_NULLABLE, 1, &valuePointer, nullptr, nullptr, nullptr};
		std::array<ArrowSchema *, 3> fields{&source, &target, &properties};
		ArrowSchema root{"+s", "edges", nullptr, 0, 3, fields.data(), nullptr, nullptr, nullptr};

		zyx_driver_error_t *error = nullptr;
		zyx_driver_ingest_t *ingest = nullptr;
		ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
		zyx_driver_edge_ingestor_t *ingestor = nullptr;
		ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "PRIMITIVE_REL", &root, &ingestor, &error), ZYX_DRIVER_OK);
		ASSERT_NE(ingestor, nullptr);
		ASSERT_EQ(error, nullptr);
		EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(error, nullptr);
		EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(error, nullptr);
	}
}

TEST_F(DriverAbiIngestSchemaValidationTest, AcceptsEveryIntegralDictionaryIndexKind) {
	const std::array<const char *, 8> indexFormats{"c", "C", "s", "S", "i", "I", "l", "L"};
	for (const char *indexFormat: indexFormats) {
		ArrowSchema source = scalar("l", "source_id");
		ArrowSchema target = scalar("l", "target_id");
		ArrowSchema dictionaryValues = scalar("u", "dictionary");
		ArrowSchema category = scalar(indexFormat, "category");
		category.dictionary = &dictionaryValues;
		ArrowSchema *categoryPointer = &category;
		ArrowSchema properties = {"+s", "properties", nullptr, ARROW_FLAG_NULLABLE, 1, &categoryPointer, nullptr,
									nullptr, nullptr};
		std::array<ArrowSchema *, 3> fields{&source, &target, &properties};
		ArrowSchema root{"+s", "edges", nullptr, 0, 3, fields.data(), nullptr, nullptr, nullptr};

		zyx_driver_error_t *error = nullptr;
		zyx_driver_ingest_t *ingest = nullptr;
		ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
		zyx_driver_edge_ingestor_t *ingestor = nullptr;
		ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "DICTIONARY_REL", &root, &ingestor, &error), ZYX_DRIVER_OK);
		ASSERT_NE(ingestor, nullptr);
		ASSERT_EQ(error, nullptr);
		EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(error, nullptr);
		EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
		EXPECT_EQ(error, nullptr);
	}
}

TEST_F(DriverAbiIngestSchemaValidationTest, AcceptsDictionaryEncodedUtf8MapKeys) {
	ArrowSchema source = scalar("l", "source_id");
	ArrowSchema target = scalar("l", "target_id");
	ArrowSchema dictionaryValues = scalar("u", "dictionary");
	ArrowSchema key = scalar("i", "key");
	key.dictionary = &dictionaryValues;
	ArrowSchema value = scalar("u", "value");
	std::array<ArrowSchema *, 2> entryFields{&key, &value};
	ArrowSchema entries{"+s", "entries", nullptr, ARROW_FLAG_NULLABLE, 2, entryFields.data(), nullptr, nullptr,
					nullptr};
	ArrowSchema *entriesPointer = &entries;
	ArrowSchema attributes{"+m", "attributes", nullptr, ARROW_FLAG_NULLABLE, 1, &entriesPointer, nullptr, nullptr,
						nullptr};
	ArrowSchema *attributesPointer = &attributes;
	ArrowSchema properties{"+s", "properties", nullptr, ARROW_FLAG_NULLABLE, 1, &attributesPointer, nullptr, nullptr,
					   nullptr};
	std::array<ArrowSchema *, 3> fields{&source, &target, &properties};
	ArrowSchema root{"+s", "edges", nullptr, 0, 3, fields.data(), nullptr, nullptr, nullptr};

	zyx_driver_error_t *error = nullptr;
	zyx_driver_ingest_t *ingest = nullptr;
	ASSERT_EQ(zyx_driver_ingest_begin(db, &ingest, &error), ZYX_DRIVER_OK);
	zyx_driver_edge_ingestor_t *ingestor = nullptr;
	ASSERT_EQ(zyx_driver_ingest_prepare_edges(ingest, "MAP_REL", &root, &ingestor, &error), ZYX_DRIVER_OK);
	ASSERT_NE(ingestor, nullptr);
	ASSERT_EQ(error, nullptr);
	EXPECT_EQ(zyx_driver_edge_ingestor_close(ingestor, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(error, nullptr);
	EXPECT_EQ(zyx_driver_ingest_close(ingest, &error), ZYX_DRIVER_OK);
	EXPECT_EQ(error, nullptr);
}
