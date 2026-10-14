#include "storage/relocatable/backend/RelocatableStorageBackendTestFixture.hpp"
using namespace relocatable_storage_backend_test;

TEST_F(RelocatableStorageBackendTest, EscapedPropertyKeysShareIdentityAcrossCreateMatchSetRemoveAndReopen) {
	materializeNativeDatabase();
	zyx::Database database(data_path_.string()); database.open();
	auto create = database.execute("CREATE (a:Escaped {`id`:1, `tick``key`:7, `with space`:3}), (b:Escaped {`id`:2}) CREATE (a)-[:ESCAPED {`weight`:9}]->(b)");
	ASSERT_TRUE(create.isSuccess()) << create.getError();
	auto read = database.execute("MATCH (a:Escaped {id:1})-[r:ESCAPED {`weight`:9}]->(b) RETURN a.`tick``key` AS value,r.weight AS weight,b.id AS id");
	ASSERT_TRUE(read.isSuccess()) << read.getError(); ASSERT_TRUE(read.hasNext()); read.next();
	EXPECT_EQ(std::get<int64_t>(read.get("value")), 7); EXPECT_EQ(std::get<int64_t>(read.get("weight")), 9);
	EXPECT_EQ(std::get<int64_t>(read.get("id")), 2); EXPECT_FALSE(read.hasNext());
	auto change = database.execute("MATCH (a:Escaped {`id`:1}) SET a.`tick``key` = 11 REMOVE a.`with space` RETURN a.`tick``key` AS value,a.`with space` AS removed");
	ASSERT_TRUE(change.isSuccess()) << change.getError(); ASSERT_TRUE(change.hasNext()); change.next();
	EXPECT_EQ(std::get<int64_t>(change.get("value")), 11); EXPECT_TRUE(std::holds_alternative<std::monostate>(change.get("removed")));
	database.close(); database.open();
	auto reopened = database.execute("MATCH (a:Escaped {id:1}) RETURN keys(a) AS keys, a.`tick``key` AS value");
	ASSERT_TRUE(reopened.isSuccess()) << reopened.getError(); ASSERT_TRUE(reopened.hasNext()); reopened.next();
	EXPECT_EQ(std::get<int64_t>(reopened.get("value")), 11);
	const auto keys = reopened.get("keys");
	std::vector<std::string> names;
	if (const auto *strings = std::get_if<std::vector<std::string>>(&keys)) names = *strings;
	else {
		const auto list = std::get<std::shared_ptr<zyx::ValueList>>(keys); ASSERT_NE(list, nullptr);
		for (const auto &entry : list->elements) names.push_back(std::get<std::string>(entry));
	}
	std::ranges::sort(names); EXPECT_EQ(names, (std::vector<std::string>{"id", "tick`key"}));
	database.close();
}
