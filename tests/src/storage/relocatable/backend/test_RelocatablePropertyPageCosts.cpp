#include <gtest/gtest.h>
#include <iostream>
#include <map>
#include <set>
#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"

using namespace relocatable_storage_backend_test;
namespace {
struct PropertyPageIO {
	uint64_t bytes = 0U, calls = 0U;
	std::set<uint64_t> pages;
	std::map<unsigned, uint64_t> ownerBytes;
	std::map<uint32_t, uint64_t> magicBytes;
	std::map<unsigned, uint64_t> directoryBytes;
	void record(const void *buffer, size_t size, pwrite_off_t offset) {
		bytes += size; ++calls;
		if (size == 0U) return;
		const auto begin = static_cast<uint64_t>(offset);
		for (uint64_t page = begin / RSS_PAGE_SIZE; page <= (begin + size - 1U) / RSS_PAGE_SIZE; ++page)
			pages.insert(page);
		unsigned kind = 0U; // Metadata, pool control, superblocks and other non-owner-header IO.
		if (size == RSS_PAGE_SIZE) {
			try {
				const auto bytesView = std::span(static_cast<const std::byte *>(buffer), size);
				kind = static_cast<unsigned>(PageOwnerHeaderCodec::decode(bytesView.first(RSS_PAGE_OWNER_HEADER_BYTES)).kind);
			} catch (const FormatException &) {}
		}
		ownerBytes[kind] += size;
		if (kind == 0U && size >= 4U) {
			const auto view = std::span(static_cast<const std::byte *>(buffer), size);
			const auto magic = LittleEndianCodec::readU32(view, 0U);
			magicBytes[magic] += size;
			if (size == RSS_PAGE_SIZE && magic == RSS_LOCATION_LEAF_MAGIC)
				directoryBytes[static_cast<unsigned>(LocationLeafPageCodec::decode(view).directory_type)] += size;
			if (size == RSS_PAGE_SIZE && magic == RSS_CATALOG_RADIX_PAGE_MAGIC)
				directoryBytes[static_cast<unsigned>(CatalogRadixPageCodec::decode(view).directory_type)] += size;
		}
	}
};
template<class Operation>
void measurePropertyIO(const std::string &shape, const char *operation, Operation run) {
	PropertyPageIO reads, writes;
	StorageIO::ScopedNativeReadHookForTesting readHook([&](file_handle_t fd, void *buffer, size_t count, pwrite_off_t offset) {
		const auto result = portable_pread(fd, buffer, count, offset);
		if (result > 0) reads.record(buffer, static_cast<size_t>(result), offset);
		return result;
	});
	StorageIO::ScopedNativeWriteHookForTesting writeHook([&](file_handle_t fd, const void *buffer, size_t count, pwrite_off_t offset) {
		const auto result = portable_pwrite(fd, buffer, count, offset);
		if (result > 0) writes.record(buffer, static_cast<size_t>(result), offset);
		return result;
	});
	run();
	EXPECT_GT(reads.bytes + writes.bytes, 0U);
	std::cout << "PROPERTY_PAGE_COST " << shape << ' ' << operation
		<< " read_bytes=" << reads.bytes << " read_calls=" << reads.calls << " read_unique_pages=" << reads.pages.size()
		<< " write_bytes=" << writes.bytes << " write_calls=" << writes.calls << " write_unique_pages=" << writes.pages.size();
	for (const auto &[kind, bytes] : reads.ownerBytes) std::cout << " read_owner_" << kind << '=' << bytes;
	for (const auto &[kind, bytes] : writes.ownerBytes) std::cout << " write_owner_" << kind << '=' << bytes;
	for (const auto &[magic, bytes] : reads.magicBytes) std::cout << " read_magic_" << magic << '=' << bytes;
	for (const auto &[magic, bytes] : writes.magicBytes) std::cout << " write_magic_" << magic << '=' << bytes;
	for (const auto &[directory, bytes] : reads.directoryBytes) std::cout << " read_directory_" << directory << '=' << bytes;
	for (const auto &[directory, bytes] : writes.directoryBytes) std::cout << " write_directory_" << directory << '=' << bytes;
	std::cout << '\n';
}
}

TEST_F(RelocatableStorageBackendTest, PropertyLayoutMeasuresActualNativePagesAcrossFiveAccessPaths) {
	for (const std::string shape : {"shared", "singleton", "wide_large"}) {
		SCOPED_TRACE(shape);
		materializeNativeDatabase();
		auto options = backendOptions(); options.open_mode = OpenMode::OPEN_EXISTING_FILE;
		options.buffer_pool_bytes = 16U * RSS_PAGE_SIZE;
		RelocatableStorageBackend backend(options); backend.open();
		std::vector<LogicalGraphNodeCreateRequest> requests;
		for (uint64_t ordinal = 0U; ordinal < 64U; ++ordinal) {
			std::vector<LogicalGraphWriteProperty> fields{{"value", PropertyValue(ordinal)}};
			if (shape == "singleton") fields.push_back({"unique_" + std::to_string(ordinal), PropertyValue(ordinal)});
			if (shape == "wide_large") {
				for (uint64_t field = 0U; field < 16U; ++field)
					fields.push_back({"field_" + std::to_string(field), PropertyValue(field + ordinal)});
				fields.push_back({"large", PropertyValue(std::string(16384U, static_cast<char>('a' + ordinal % 26U)))});
			}
			requests.push_back({{"Cost"}, std::move(fields)});
		}
		const auto ids = backend.createLogicalNodes(requests); ASSERT_EQ(ids.size(), 64U);
		backend.close(); backend.open();
		measurePropertyIO(shape, "point", [&] {
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			EXPECT_EQ(snapshot->readNodeProperty(ids[31U], "value"), PropertyValue(uint64_t(31U)));
		});
		backend.close(); backend.open();
		measurePropertyIO(shape, "full", [&] {
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			const auto node = snapshot->readNode(ids[31U]);
			EXPECT_EQ(node.properties.size(), requests[31U].properties.size());
			for (const auto &expected : requests[31U].properties) {
				const auto found = std::ranges::find(node.properties, expected.key, &LogicalGraphPropertyEntry::key);
				ASSERT_NE(found, node.properties.end()); EXPECT_EQ(found->value, expected.value);
			}
		});
		backend.close(); backend.open();
		measurePropertyIO(shape, "scan_one_field", [&] {
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			const std::array<std::string_view, 1U> keys{"value"};
			const auto columns = snapshot->readNodePropertyColumnsSorted(ids, keys);
			ASSERT_EQ(columns.size(), 1U); ASSERT_EQ(columns[0].size(), ids.size());
			for (size_t ordinal = 0U; ordinal < ids.size(); ++ordinal)
				EXPECT_EQ(columns[0][ordinal], PropertyValue(static_cast<uint64_t>(ordinal)));
		});
		backend.close(); backend.open();
		measurePropertyIO(shape, "set_type_change", [&] {
			auto transaction = backend.beginLogicalGraphTransaction(false);
			std::vector<LogicalGraphPropertyPatchRequest> change{{{LogicalGraphMutationEntityKind::LGMK_NODE,
				LogicalGraphMutationReferenceKind::LGRK_PERSISTED, ids[31U]}, {{"value", PropertyValue("changed")}}, {}}};
			(void) transaction->stageMutationBatchWithPropertyPatches({}, std::move(change), 1000U);
			(void) transaction->commit();
		});
		{
			auto snapshot = backend.openLogicalGraphReadSnapshot();
			EXPECT_EQ(snapshot->readNodeProperty(ids[31U], "value"), PropertyValue("changed"));
			EXPECT_EQ(snapshot->readNodeProperty(ids[30U], "value"), PropertyValue(uint64_t(30U)));
		}
		backend.close(); backend.open();
		measurePropertyIO(shape, "delete_retire", [&] { backend.deleteLogicalNodes({{ids[31U], false, 0U}}); });
		EXPECT_NO_THROW(static_cast<void>(backend.verifyIntegrity())); backend.close(); backend.open();
		{ auto snapshot = backend.openLogicalGraphReadSnapshot(); EXPECT_FALSE(snapshot->containsNode(ids[31U])); }
		backend.close();
		std::filesystem::remove(data_path_);
	}
}
