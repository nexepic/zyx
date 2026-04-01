/**
 * @file test_ApiTransaction.cpp
 * @author Nexepic
 * @date 2026/1/5
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include <variant>

#include "ApiTestFixture.hpp"

// ============================================================================
// Transaction API Tests
// ============================================================================

TEST_F(CppApiTest, TransactionBeginCommit) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:TxnTest {name: 'committed'})");
	EXPECT_TRUE(res.isSuccess());

	txn.commit();
	EXPECT_FALSE(txn.isActive());

	auto queryRes = db->execute("MATCH (n:TxnTest) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "committed");
}

TEST_F(CppApiTest, TransactionRollback) {
	db->createNode("Existing", {{"id", (int64_t) 1}});
	db->save();

	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:RollbackTest {name: 'should_disappear'})");
	EXPECT_TRUE(res.isSuccess());

	txn.rollback();
	EXPECT_FALSE(txn.isActive());
}

TEST_F(CppApiTest, TransactionExecuteWhenNotActive) {
	auto txn = db->beginTransaction();
	txn.commit();
	EXPECT_FALSE(txn.isActive());

	auto res = txn.execute("MATCH (n) RETURN n");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
}

TEST_F(CppApiTest, TransactionMoveSemantics) {
	auto txn1 = db->beginTransaction();
	EXPECT_TRUE(txn1.isActive());

	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_TRUE(txn2.isActive());
	EXPECT_FALSE(txn1.isActive());

	txn2.rollback();
	EXPECT_FALSE(txn2.isActive());
}

TEST_F(CppApiTest, BeginTransactionOnClosedDb) {
	db->close();

	auto tempDir = fs::temp_directory_path();
	std::string newPath = (tempDir / ("api_txn_test_" + std::to_string(std::rand()))).string();
	auto newDb = std::make_unique<zyx::Database>(newPath);

	auto txn = newDb->beginTransaction();
	EXPECT_TRUE(txn.isActive());
	txn.rollback();

	newDb->close();
	if (fs::exists(newPath))
		fs::remove_all(newPath);
}

TEST_F(CppApiTest, TransactionExecuteAndQueryData) {
	auto txn = db->beginTransaction();

	auto createRes = txn.execute("CREATE (n:TxnData {name: 'txn_node', value: 42})");
	EXPECT_TRUE(createRes.isSuccess());

	auto queryRes = txn.execute("MATCH (n:TxnData) RETURN n.name, n.value");
	EXPECT_TRUE(queryRes.isSuccess());

	if (queryRes.hasNext()) {
		queryRes.next();
		auto name = queryRes.get("n.name");
		EXPECT_TRUE(std::holds_alternative<std::string>(name));
	}

	txn.commit();
}

TEST_F(CppApiTest, TransactionCommitOnMovedFrom) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_THROW(txn1.commit(), std::runtime_error);
	txn2.rollback();
}

TEST_F(CppApiTest, TransactionRollbackOnMovedFrom) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	EXPECT_THROW(txn1.rollback(), std::runtime_error);
	txn2.rollback();
}

TEST_F(CppApiTest, ExecuteWithExplicitTransaction) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = db->execute("RETURN 1 AS val");
	(void)res;

	txn.rollback();
}

TEST_F(CppApiTest, TransactionWithDeleteAndCommit) {
	db->createNode("TxnDel", {{"name", "alice"}});
	db->createNode("TxnDel", {{"name", "bob"}});
	db->save();

	auto txn = db->beginTransaction();
	auto res = txn.execute("MATCH (n:TxnDel {name: 'alice'}) DELETE n");
	EXPECT_TRUE(res.isSuccess());
	txn.commit();

	auto queryRes = db->execute("MATCH (n:TxnDel) RETURN n.name");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto nameVal = queryRes.get("n.name");
	EXPECT_TRUE(std::holds_alternative<std::string>(nameVal));
	EXPECT_EQ(std::get<std::string>(nameVal), "bob");
	EXPECT_FALSE(queryRes.hasNext());
}

TEST_F(CppApiTest, TransactionExecuteSuccessWithDuration) {
	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res = txn.execute("CREATE (n:TxnDuration {val: 42}) RETURN n.val");
	EXPECT_TRUE(res.isSuccess());
	EXPECT_GE(res.getDuration(), 0.0);

	if (res.hasNext()) {
		res.next();
		auto val = res.get("n.val");
		EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	}

	txn.commit();
}

TEST_F(CppApiTest, TransactionMoveAssignmentOperator) {
	auto txn1 = db->beginTransaction();
	txn1.commit();

	auto txn2 = db->beginTransaction();
	EXPECT_TRUE(txn2.isActive());

	txn1 = std::move(txn2);
	EXPECT_TRUE(txn1.isActive());
	EXPECT_FALSE(txn2.isActive());

	txn1.rollback();
}

TEST_F(CppApiTest, TransactionExecuteOnMovedFromReturnsError) {
	auto txn1 = db->beginTransaction();
	zyx::Transaction txn2 = std::move(txn1);
	auto res = txn1.execute("RETURN 1");
	EXPECT_FALSE(res.isSuccess());
	EXPECT_EQ(res.getError(), "Transaction is not active");
	EXPECT_FALSE(res.hasNext());
	txn2.rollback();
}

TEST_F(CppApiTest, TransactionExecuteErrorPaths) {
	auto txn = db->beginTransaction();

	auto res = txn.execute("COMPLETELY INVALID !@#$%^&*()");
	EXPECT_FALSE(res.isSuccess());
	auto err = res.getError();
	EXPECT_FALSE(err.empty());

	EXPECT_FALSE(res.hasNext());
	EXPECT_EQ(res.getColumnCount(), 0);
	EXPECT_EQ(res.getDuration(), 0.0);

	auto val = res.get("anything");
	EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

	auto valIdx = res.get(0);
	EXPECT_TRUE(std::holds_alternative<std::monostate>(valIdx));

	txn.rollback();
}

TEST_F(CppApiTest, HasActiveTransactionBranch) {
	auto res1 = db->execute("RETURN 1 AS val");
	EXPECT_TRUE(res1.isSuccess());

	auto txn = db->beginTransaction();
	EXPECT_TRUE(txn.isActive());

	auto res2 = db->execute("RETURN 2 AS val");
	(void)res2;

	txn.rollback();
}

TEST_F(CppApiTest, ExecuteImplicitTransactionCommit) {
	auto res = db->execute("CREATE (n:ImplTxn {val: 99}) RETURN n.val");
	ASSERT_TRUE(res.isSuccess());

	auto queryRes = db->execute("MATCH (n:ImplTxn) RETURN n.val");
	ASSERT_TRUE(queryRes.hasNext());
	queryRes.next();
	auto val = queryRes.get("n.val");
	EXPECT_TRUE(std::holds_alternative<int64_t>(val));
	EXPECT_EQ(std::get<int64_t>(val), 99);
}
