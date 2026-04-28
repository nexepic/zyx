#include <gtest/gtest.h>
#include <CLI/CLI.hpp>
#include "graph/cli/ImportCommand.hpp"
#include "graph/core/Database.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace graph;
using namespace graph::cli;

class ImportCommandTest : public ::testing::Test {
protected:
    void SetUp() override {
        dbPath = std::filesystem::temp_directory_path() / "test_import_db";
        std::filesystem::remove_all(dbPath);
        
        nodeFile = std::filesystem::temp_directory_path() / "nodes.csv";
        edgeFile = std::filesystem::temp_directory_path() / "edges.csv";
        jsonlNodeFile = std::filesystem::temp_directory_path() / "nodes.jsonl";
        jsonlEdgeFile = std::filesystem::temp_directory_path() / "edges.jsonl";
        invalidFile = std::filesystem::temp_directory_path() / "invalid.csv";
    }

    void TearDown() override {
        std::filesystem::remove_all(dbPath);
        std::filesystem::remove(nodeFile);
        std::filesystem::remove(edgeFile);
        std::filesystem::remove(jsonlNodeFile);
        std::filesystem::remove(jsonlEdgeFile);
        std::filesystem::remove(invalidFile);
    }
    
    void writeCsv(const std::filesystem::path& path, const std::string& content) {
        std::ofstream f(path);
        f << content;
    }

    std::filesystem::path dbPath;
    std::filesystem::path nodeFile;
    std::filesystem::path edgeFile;
    std::filesystem::path jsonlNodeFile;
    std::filesystem::path jsonlEdgeFile;
    std::filesystem::path invalidFile;
};

TEST_F(ImportCommandTest, BasicCsvImport) {
    writeCsv(nodeFile, ":ID,:LABEL,name,age:INT\n1,Person,Alice,30\n2,Person,Bob,40\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE,weight:FLOAT\n1,2,KNOWS,1.5\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --rels " + edgeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    // Verify database contents
    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "2");
    
    auto edgeResult = db.getQueryEngine()->execute("MATCH ()-[r]->() RETURN COUNT(r) as c");
    EXPECT_EQ(edgeResult.getRows()[0].at("c").asPrimitive().toString(), "1");
}

TEST_F(ImportCommandTest, JsonlImport) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\"], \"name\": \"Alice\", \"age\": 30}\n"
                            "{\"_id\": \"2\", \"_labels\": [\"Person\"], \"name\": \"Bob\", \"age\": 40}\n");
    writeCsv(jsonlEdgeFile, "{\"_start\": \"1\", \"_end\": \"2\", \"_type\": \"KNOWS\", \"weight\": 1.5}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() + " --rels " + jsonlEdgeFile.string() + " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "2");
}

TEST_F(ImportCommandTest, SkipBadEntries) {
    writeCsv(invalidFile, ":ID,:LABEL,age:INT\n1,Person,30\n2,Person\n3,Person,40\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + invalidFile.string() + " --db " + dbPath.string() + " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "2"); // Node 2 skipped
}

TEST_F(ImportCommandTest, IdGroupAndArrayTypes) {
    writeCsv(nodeFile, ":ID(Person),:LABEL,tags:STRING[]\nP1,Person,a;b;c\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.tags as tags");
    auto tags = result.getRows()[0].at("tags").asPrimitive().getList();
    EXPECT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0].toString(), "a");
}

// ---------- Header parsing: :START_ID(Group) and :END_ID(Group) ----------

TEST_F(ImportCommandTest, StartIdAndEndIdWithGroups) {
    // Use :START_ID(Person) and :END_ID(Person) to cover group parsing for these headers
    writeCsv(nodeFile, ":ID(Person),:LABEL,name\n1,Person,Alice\n2,Person,Bob\n");
    writeCsv(edgeFile, ":START_ID(Person),:END_ID(Person),:TYPE\n1,2,KNOWS\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r:KNOWS]->() RETURN COUNT(r) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- Header parsing: named ID columns "personId:ID" and "personId:ID(Group)" ----------

TEST_F(ImportCommandTest, NamedIdColumn) {
    // "personId:ID" should be treated as an ID column (covers lines 79-85)
    writeCsv(nodeFile, "personId:ID,:LABEL,name\nP1,Person,Alice\nP2,Person,Bob\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\nP1,P2,KNOWS\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r:KNOWS]->() RETURN COUNT(r) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

TEST_F(ImportCommandTest, NamedIdColumnWithGroup) {
    // "personId:ID(People)" covers the ID(Group) branch in property parsing (lines 81-83)
    writeCsv(nodeFile, "personId:ID(People),:LABEL,name\nP1,Person,Alice\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- convertValue: various array element types ----------

TEST_F(ImportCommandTest, IntArrayType) {
    writeCsv(nodeFile, ":ID,:LABEL,scores:INT[]\n1,Person,10;20;30\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.scores as s");
    auto arr = result.getRows()[0].at("s").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0].toString(), "10");
}

TEST_F(ImportCommandTest, FloatArrayType) {
    writeCsv(nodeFile, ":ID,:LABEL,weights:FLOAT[]\n1,Person,1.1;2.2;3.3\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.weights as w");
    auto arr = result.getRows()[0].at("w").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 3u);
}

TEST_F(ImportCommandTest, BooleanArrayType) {
    writeCsv(nodeFile, ":ID,:LABEL,flags:BOOLEAN[]\n1,Person,true;FALSE;1\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.flags as f");
    auto arr = result.getRows()[0].at("f").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 3u);
}

// ---------- convertValue: INT array parse error fallback ----------

TEST_F(ImportCommandTest, IntArrayParseErrorFallback) {
    // "abc" cannot be parsed as int64, should fall back to string element
    writeCsv(nodeFile, ":ID,:LABEL,data:INT[]\n1,Person,abc;def\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.data as d");
    auto arr = result.getRows()[0].at("d").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 2u);
    EXPECT_EQ(arr[0].toString(), "abc");
}

TEST_F(ImportCommandTest, FloatArrayParseErrorFallback) {
    writeCsv(nodeFile, ":ID,:LABEL,vals:DOUBLE[]\n1,Person,not_a_num;xyz\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.vals as v");
    auto arr = result.getRows()[0].at("v").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 2u);
    EXPECT_EQ(arr[0].toString(), "not_a_num");
}

// ---------- convertValue: empty value returns null (property skipped) ----------

TEST_F(ImportCommandTest, EmptyValueSkipped) {
    // Empty field should produce NULL_TYPE and be skipped in properties
    writeCsv(nodeFile, ":ID,:LABEL,name,age:INT\n1,Person,,30\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.name as name, n.age as age");
    EXPECT_EQ(result.rowCount(), 1u);
    // name should be null (not set), age should be 30
    EXPECT_EQ(result.getRows()[0].at("age").asPrimitive().toString(), "30");
}

// ---------- convertValue: scalar type conversions ----------

TEST_F(ImportCommandTest, ScalarTypeConversions) {
    // Cover INT, FLOAT, DOUBLE, BOOLEAN scalar types and the LONG alias
    writeCsv(nodeFile, ":ID,:LABEL,age:INT,height:FLOAT,weight:DOUBLE,active:BOOLEAN,count:LONG\n"
                       "1,Person,30,5.9,150.5,true,100\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute(
        "MATCH (n:Person) RETURN n.age as age, n.height as h, n.weight as w, n.active as a, n.count as c");
    EXPECT_EQ(result.rowCount(), 1u);
    auto& row = result.getRows()[0];
    EXPECT_EQ(row.at("age").asPrimitive().toString(), "30");
    EXPECT_EQ(row.at("a").asPrimitive().toString(), "true");
    EXPECT_EQ(row.at("c").asPrimitive().toString(), "100");
}

TEST_F(ImportCommandTest, ScalarIntParseErrorFallback) {
    // Non-numeric value for INT should fall back to string
    writeCsv(nodeFile, ":ID,:LABEL,age:INT\n1,Person,not_a_number\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.age as age");
    EXPECT_EQ(result.getRows()[0].at("age").asPrimitive().toString(), "not_a_number");
}

TEST_F(ImportCommandTest, ScalarFloatParseErrorFallback) {
    writeCsv(nodeFile, ":ID,:LABEL,val:FLOAT\n1,Person,not_a_float\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.val as v");
    EXPECT_EQ(result.getRows()[0].at("v").asPrimitive().toString(), "not_a_float");
}

TEST_F(ImportCommandTest, BooleanFalseValue) {
    // "false" should produce false (not matching "true", "TRUE", or "1")
    writeCsv(nodeFile, ":ID,:LABEL,active:BOOLEAN\n1,Person,false\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.active as a");
    EXPECT_EQ(result.getRows()[0].at("a").asPrimitive().toString(), "false");
}

// ---------- Node with no label gets default ":Node" ----------

TEST_F(ImportCommandTest, DefaultNodeLabel) {
    writeCsv(nodeFile, ":ID,name\n1,Alice\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Node) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- Relationship CSV: properties on edges ----------

TEST_F(ImportCommandTest, RelationshipCsvWithProperties) {
    writeCsv(nodeFile, ":ID,:LABEL,name\n1,Person,Alice\n2,Person,Bob\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE,since:INT,note\n1,2,KNOWS,2020,college\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r:KNOWS]->() RETURN r.since as s, r.note as n");
    EXPECT_EQ(result.rowCount(), 1u);
    EXPECT_EQ(result.getRows()[0].at("s").asPrimitive().toString(), "2020");
    EXPECT_EQ(result.getRows()[0].at("n").asPrimitive().toString(), "college");
}

// ---------- Relationship CSV: unresolvable IDs ----------

TEST_F(ImportCommandTest, RelationshipCsvUnresolvableIdsSkip) {
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n1,999,KNOWS\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string() +
                      " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));
}

TEST_F(ImportCommandTest, RelationshipCsvUnresolvableIdsThrow) {
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n1,999,KNOWS\n");

    CLI::App app;
    registerImportCommand(app);

    // Without --skip-bad-entries, error is caught by the callback's try/catch
    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- Relationship CSV: row field count mismatch ----------

TEST_F(ImportCommandTest, RelationshipCsvFieldMismatchSkip) {
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n2,Person\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n1,2\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string() +
                      " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- JSONL import: auto-detect format from extension ----------

TEST_F(ImportCommandTest, AutoDetectJsonlFormat) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\"]}\n"
                            "{\"_id\": \"2\", \"_labels\": [\"Person\"]}\n");
    writeCsv(jsonlEdgeFile, "{\"_start\": \"1\", \"_end\": \"2\", \"_type\": \"KNOWS\"}\n");

    CLI::App app;
    registerImportCommand(app);

    // Use --format auto (default) — should detect .jsonl extension
    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "2");
    auto edgeResult = db.getQueryEngine()->execute("MATCH ()-[r]->() RETURN COUNT(r) as c");
    EXPECT_EQ(edgeResult.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL: auto-detect from .json extension ----------

TEST_F(ImportCommandTest, AutoDetectJsonFormat) {
    auto jsonFile = std::filesystem::temp_directory_path() / "nodes.json";
    writeCsv(jsonFile, "{\"_id\": \"1\", \"_labels\": [\"Animal\"]}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Animal) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");

    std::filesystem::remove(jsonFile);
}

// ---------- JSONL: node with no _id and no _labels (default label) ----------

TEST_F(ImportCommandTest, JsonlNodeNoIdNoLabels) {
    writeCsv(jsonlNodeFile, "{\"name\": \"Alice\"}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    // Should get default label ":Node"
    auto result = db.getQueryEngine()->execute("MATCH (n:Node) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL: empty lines in file ----------

TEST_F(ImportCommandTest, JsonlEmptyLines) {
    writeCsv(jsonlNodeFile, "\n{\"_id\": \"1\", \"_labels\": [\"Person\"]}\n\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL: relationship with unresolvable IDs ----------

TEST_F(ImportCommandTest, JsonlRelUnresolvableSkip) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\"]}\n");
    writeCsv(jsonlEdgeFile, "{\"_start\": \"1\", \"_end\": \"999\", \"_type\": \"KNOWS\"}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string() +
                      " --format jsonl --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));
}

TEST_F(ImportCommandTest, JsonlRelUnresolvableThrow) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\"]}\n");
    writeCsv(jsonlEdgeFile, "{\"_start\": \"1\", \"_end\": \"999\", \"_type\": \"KNOWS\"}\n");

    CLI::App app;
    registerImportCommand(app);

    // Error is caught by callback's try/catch, so no throw at app.parse level
    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string() +
                      " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- JSONL: empty lines in relationship file ----------

TEST_F(ImportCommandTest, JsonlRelEmptyLines) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\"]}\n"
                            "{\"_id\": \"2\", \"_labels\": [\"Person\"]}\n");
    writeCsv(jsonlEdgeFile, "\n{\"_start\": \"1\", \"_end\": \"2\", \"_type\": \"KNOWS\"}\n\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r]->() RETURN COUNT(r) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL: multiple labels ----------

TEST_F(ImportCommandTest, JsonlMultipleLabels) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\", \"Employee\"]}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person:Employee) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- CSV: multiple labels via semicolon ----------

TEST_F(ImportCommandTest, CsvMultipleLabels) {
    writeCsv(nodeFile, ":ID,:LABEL,name\n1,Person;Employee,Alice\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person:Employee) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- CSV: property with single quotes (formatValueForCypher escaping) ----------

TEST_F(ImportCommandTest, PropertyWithSingleQuote) {
    writeCsv(nodeFile, ":ID,:LABEL,name\n1,Person,O'Brien\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.name as name");
    EXPECT_EQ(result.getRows()[0].at("name").asPrimitive().toString(), "O'Brien");
}

// ---------- CSV: custom array delimiter ----------

TEST_F(ImportCommandTest, CustomArrayDelimiter) {
    writeCsv(nodeFile, ":ID,:LABEL,tags:STRING[]\n1,Person,a|b|c\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --db " + dbPath.string() +
                      " --array-delimiter |";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.tags as t");
    auto arr = result.getRows()[0].at("t").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[1].toString(), "b");
}

// ---------- Nodes-only import (no relationship files) ----------

TEST_F(ImportCommandTest, NodesOnlyImport) {
    writeCsv(nodeFile, ":ID,:LABEL,name\n1,Person,Alice\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- Explicit --format csv ----------

TEST_F(ImportCommandTest, ExplicitCsvFormat) {
    writeCsv(nodeFile, ":ID,:LABEL,name\n1,Person,Alice\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --db " + dbPath.string() + " --format csv";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- INTEGER and LONG type aliases for array ----------

TEST_F(ImportCommandTest, IntegerAndLongArrayAliases) {
    writeCsv(nodeFile, ":ID,:LABEL,a:INTEGER[],b:LONG[]\n1,Person,1;2,3;4\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.a as a, n.b as b");
    EXPECT_EQ(result.getRows()[0].at("a").asPrimitive().getList().size(), 2u);
    EXPECT_EQ(result.getRows()[0].at("b").asPrimitive().getList().size(), 2u);
}

// ---------- DOUBLE array type ----------

TEST_F(ImportCommandTest, DoubleArrayType) {
    writeCsv(nodeFile, ":ID,:LABEL,vals:DOUBLE[]\n1,Person,1.1;2.2\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.vals as v");
    EXPECT_EQ(result.getRows()[0].at("v").asPrimitive().getList().size(), 2u);
}

// ---------- Empty CSV files (header-only or truly empty) ----------

TEST_F(ImportCommandTest, EmptyCsvNodeFile) {
    // Completely empty CSV file should throw "Empty CSV file"
    writeCsv(nodeFile, "");

    CLI::App app;
    registerImportCommand(app);

    // The callback catches the exception internally, so app.parse won't throw
    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

TEST_F(ImportCommandTest, EmptyCsvRelFile) {
    // Empty relationship CSV should throw "Empty CSV file" (caught by callback)
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n");
    writeCsv(edgeFile, "");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- Row field count mismatch without --skip-bad-entries (throw path) ----------

TEST_F(ImportCommandTest, NodeCsvFieldMismatchThrow) {
    // Row has fewer fields than header, without --skip-bad-entries → throws (caught by callback)
    writeCsv(nodeFile, ":ID,:LABEL,name,age:INT\n1,Person\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

TEST_F(ImportCommandTest, RelCsvFieldMismatchThrow) {
    // Relationship row field count mismatch without skip → throws (caught by callback)
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n2,Person\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n1,2\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- BOOLEAN array: "TRUE" uppercase variant ----------

TEST_F(ImportCommandTest, BooleanArrayTrueUppercase) {
    // Cover the "TRUE" branch at line 115 (elem == "TRUE")
    writeCsv(nodeFile, ":ID,:LABEL,flags:BOOLEAN[]\n1,Person,TRUE;true;1;FALSE;false;0\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.flags as f");
    auto arr = result.getRows()[0].at("f").asPrimitive().getList();
    EXPECT_EQ(arr.size(), 6u);
    // TRUE, true, 1 should all be true
    EXPECT_EQ(arr[0].toString(), "true");
    EXPECT_EQ(arr[1].toString(), "true");
    EXPECT_EQ(arr[2].toString(), "true");
    // FALSE, false, 0 should all be false
    EXPECT_EQ(arr[3].toString(), "false");
    EXPECT_EQ(arr[4].toString(), "false");
    EXPECT_EQ(arr[5].toString(), "false");
}

// ---------- Scalar INTEGER alias ----------

TEST_F(ImportCommandTest, ScalarIntegerAlias) {
    // "INTEGER" type alias for scalar (covers branch 124:27)
    writeCsv(nodeFile, ":ID,:LABEL,count:INTEGER\n1,Person,42\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.count as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "42");
}

// ---------- Scalar DOUBLE type ----------

TEST_F(ImportCommandTest, ScalarDoubleType) {
    // "DOUBLE" scalar type (covers branches 128-131)
    writeCsv(nodeFile, ":ID,:LABEL,weight:DOUBLE\n1,Person,72.5\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.weight as w");
    EXPECT_EQ(result.rowCount(), 1u);
}

// ---------- Scalar DOUBLE parse error fallback ----------

TEST_F(ImportCommandTest, ScalarDoubleParseErrorFallback) {
    writeCsv(nodeFile, ":ID,:LABEL,val:DOUBLE\n1,Person,not_double\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.val as v");
    EXPECT_EQ(result.getRows()[0].at("v").asPrimitive().toString(), "not_double");
}

// ---------- BOOLEAN scalar: "TRUE" and "1" variants ----------

TEST_F(ImportCommandTest, BooleanTrueUppercase) {
    writeCsv(nodeFile, ":ID,:LABEL,active:BOOLEAN\n1,Person,TRUE\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.active as a");
    EXPECT_EQ(result.getRows()[0].at("a").asPrimitive().toString(), "true");
}

TEST_F(ImportCommandTest, BooleanOneValue) {
    writeCsv(nodeFile, ":ID,:LABEL,active:BOOLEAN\n1,Person,1\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN n.active as a");
    EXPECT_EQ(result.getRows()[0].at("a").asPrimitive().toString(), "true");
}

// ---------- JSONL: node with _id but no _labels (tests idPos found but labelsPos not) ----------

TEST_F(ImportCommandTest, JsonlNodeIdButNoLabels) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"x1\"}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Node) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL: node with _labels but no _id (tests labelsPos found but idPos not) ----------

TEST_F(ImportCommandTest, JsonlNodeLabelsButNoId) {
    writeCsv(jsonlNodeFile, "{\"_labels\": [\"Animal\"]}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Animal) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- Header-only CSV (has header but no data rows) ----------

TEST_F(ImportCommandTest, CsvHeaderOnlyNodeFile) {
    writeCsv(nodeFile, ":ID,:LABEL,name\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() + " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "0");
}

TEST_F(ImportCommandTest, CsvHeaderOnlyRelFile) {
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r]->() RETURN COUNT(r) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "0");
}

// ---------- Relationship CSV: empty property value skipped ----------

TEST_F(ImportCommandTest, RelCsvEmptyPropertySkipped) {
    writeCsv(nodeFile, ":ID,:LABEL\n1,Person\n2,Person\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE,note\n1,2,KNOWS,\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r:KNOWS]->() RETURN COUNT(r) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- Explicit --format jsonl with CSV extension (force format) ----------

TEST_F(ImportCommandTest, ExplicitJsonlFormatOverride) {
    // Write JSONL content into a .csv file but force --format jsonl
    writeCsv(nodeFile, "{\"_id\": \"1\", \"_labels\": [\"Person\"]}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL relationship: explicit format for rel files ----------

TEST_F(ImportCommandTest, ExplicitJsonlFormatRel) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"a\", \"_labels\": [\"X\"]}\n"
                            "{\"_id\": \"b\", \"_labels\": [\"X\"]}\n");
    writeCsv(jsonlEdgeFile, "{\"_start\": \"a\", \"_end\": \"b\", \"_type\": \"LINKED\"}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string() + " --format jsonl";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH ()-[r:LINKED]->() RETURN COUNT(r) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- CSV node creation failure with --skip-bad-entries ----------

TEST_F(ImportCommandTest, CsvNodeCreateFailureSkip) {
    // Label with hyphen causes syntax error in generated CREATE query
    writeCsv(nodeFile, ":ID,:LABEL,name:STRING\n1,Person-Employee,Alice\n2,Person,Bob\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --db " + dbPath.string() + " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));

    // The valid row (Bob) should still be imported
    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- CSV node creation failure WITHOUT --skip-bad-entries ----------

TEST_F(ImportCommandTest, CsvNodeCreateFailureNoSkip) {
    // Label with hyphen causes syntax error; without skip flag, error is caught
    // at the top-level try/catch in the callback which prints to stderr
    writeCsv(nodeFile, ":ID,:LABEL,name:STRING\n1,Person-Employee,Alice\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --db " + dbPath.string();
    // The callback catches all exceptions at the top level, so parse itself won't throw
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- CSV rel creation failure with --skip-bad-entries ----------

TEST_F(ImportCommandTest, CsvRelCreateFailureSkip) {
    writeCsv(nodeFile, ":ID,:LABEL,name:STRING\n1,Person,Alice\n2,Person,Bob\n");
    // Relationship type with hyphen causes syntax error
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n1,2,KNOWS-WELL\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string() + " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));

    // Nodes should still be created, but the relationship should be skipped
    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Person) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "2");
}

// ---------- CSV rel creation failure WITHOUT --skip-bad-entries ----------

TEST_F(ImportCommandTest, CsvRelCreateFailureNoSkip) {
    writeCsv(nodeFile, ":ID,:LABEL,name:STRING\n1,Person,Alice\n2,Person,Bob\n");
    writeCsv(edgeFile, ":START_ID,:END_ID,:TYPE\n1,2,KNOWS-WELL\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nodeFile.string() +
                      " --rels " + edgeFile.string() +
                      " --db " + dbPath.string();
    // Top-level catch in callback handles the exception
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- JSONL node creation failure with --skip-bad-entries ----------

TEST_F(ImportCommandTest, JsonlNodeCreateFailureSkip) {
    // Label with hyphen causes syntax error in generated query
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Bad-Label\"]}\n"
                            "{\"_id\": \"2\", \"_labels\": [\"Good\"]}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string() + " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:Good) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "1");
}

// ---------- JSONL node creation failure WITHOUT --skip-bad-entries ----------

TEST_F(ImportCommandTest, JsonlNodeCreateFailureNoSkip) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"Bad-Label\"]}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- JSONL rel creation failure with --skip-bad-entries ----------

TEST_F(ImportCommandTest, JsonlRelCreateFailureSkip) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"a\", \"_labels\": [\"X\"]}\n"
                            "{\"_id\": \"b\", \"_labels\": [\"X\"]}\n");
    // Relationship type with hyphen causes syntax error
    writeCsv(jsonlEdgeFile, "{\"_start\": \"a\", \"_end\": \"b\", \"_type\": \"BAD-TYPE\"}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string() + " --skip-bad-entries";
    EXPECT_NO_THROW(app.parse(cmd, true));

    Database db(dbPath.string(), storage::OpenMode::OPEN_CREATE_OR_OPEN_FILE);
    db.open();
    auto result = db.getQueryEngine()->execute("MATCH (n:X) RETURN COUNT(n) as c");
    EXPECT_EQ(result.getRows()[0].at("c").asPrimitive().toString(), "2");
}

// ---------- JSONL rel creation failure WITHOUT --skip-bad-entries ----------

TEST_F(ImportCommandTest, JsonlRelCreateFailureNoSkip) {
    writeCsv(jsonlNodeFile, "{\"_id\": \"a\", \"_labels\": [\"X\"]}\n"
                            "{\"_id\": \"b\", \"_labels\": [\"X\"]}\n");
    writeCsv(jsonlEdgeFile, "{\"_start\": \"a\", \"_end\": \"b\", \"_type\": \"BAD-TYPE\"}\n");

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + jsonlEdgeFile.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- JSONL node file open failure ----------

TEST_F(ImportCommandTest, JsonlNodeFileNotFound) {
    auto nonexistent = std::filesystem::temp_directory_path() / "nonexistent_nodes.jsonl";
    std::filesystem::remove(nonexistent); // ensure it doesn't exist

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + nonexistent.string() +
                      " --db " + dbPath.string();
    // Top-level catch handles the "Cannot open JSONL file" error
    EXPECT_NO_THROW(app.parse(cmd, true));
}

// ---------- JSONL rel file open failure ----------

TEST_F(ImportCommandTest, JsonlRelFileNotFound) {
    // First import valid nodes, then try to import rels from nonexistent file
    writeCsv(jsonlNodeFile, "{\"_id\": \"1\", \"_labels\": [\"X\"]}\n");
    auto nonexistentRel = std::filesystem::temp_directory_path() / "nonexistent_rels.jsonl";
    std::filesystem::remove(nonexistentRel);

    CLI::App app;
    registerImportCommand(app);

    std::string cmd = "zyx import --nodes " + jsonlNodeFile.string() +
                      " --rels " + nonexistentRel.string() +
                      " --db " + dbPath.string();
    EXPECT_NO_THROW(app.parse(cmd, true));
}
