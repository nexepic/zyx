// Untimed correctness worker. Each input line yields exactly one complete JSON response.
#include <cmath>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include "graph/query/api/QueryEngine.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/storage/LogicalGraphTransactionOverlay.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp"

using namespace graph;
using namespace graph::storage;
using namespace graph::query;
namespace {
std::string quoted(std::string_view value) {
	std::ostringstream out; out << '"';
	for (unsigned char c : value) {
		if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
		else if (c < 32U) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
		else out << static_cast<char>(c);
	}
	out << '"'; return out.str();
}
void scalar(std::ostream &out, const PropertyValue &value) {
	out << '[';
	switch (value.getType()) {
		case PropertyType::NULL_TYPE: out << "\"null\",null"; break;
		case PropertyType::BOOLEAN: out << "\"boolean\"," << (std::get<bool>(value.getVariant()) ? "true" : "false"); break;
		case PropertyType::INTEGER: out << "\"integer\"," << std::get<int64_t>(value.getVariant()); break;
		case PropertyType::DOUBLE: {
			const auto number = std::get<double>(value.getVariant()); if (!std::isfinite(number)) throw std::runtime_error("nonfinite observer value");
			out << "\"double\"," << std::setprecision(17) << number; break;
		}
		case PropertyType::STRING: out << "\"string\"," << quoted(std::get<std::string>(value.getVariant())); break;
		case PropertyType::LIST: {
			out << "\"list\",["; bool first = true;
			for (const auto &entry : value.getList()) { if (!first) out << ','; first = false; scalar(out, entry); }
			out << ']'; break;
		}
		case PropertyType::MAP: {
			out << "\"map\",{"; bool first = true;
			std::map<std::string, PropertyValue> ordered(value.getMap().begin(), value.getMap().end());
			for (const auto &[key, entry] : ordered) { if (!first) out << ','; first = false; out << quoted(key) << ':'; scalar(out, entry); }
			out << '}'; break;
		}
		default: throw std::runtime_error("unsupported typed observer cell");
	}
	out << ']';
}
std::string rows(const QueryResult &result) {
	std::ostringstream out; out << "{\"ok\":true,\"columns\":["; bool first = true;
	for (const auto &column : result.getColumns()) { if (!first) out << ','; first = false; out << quoted(column); }
	out << "],\"rows\":["; first = true;
	for (const auto &row : result.getRows()) {
		if (!first) out << ','; first = false; out << '['; bool firstCell = true;
		for (const auto &column : result.getColumns()) {
			if (!firstCell) out << ','; firstCell = false;
			const auto &value = row.at(column);
			if (value.isPrimitive()) scalar(out, value.asPrimitive());
			else if (value.isLogicalNode()) {
				out << "[\"node\",{\"internal_id\":" << value.asLogicalNode().node_id << ",\"properties\":";
				scalar(out, PropertyValue(value.asLogicalNode().properties)); out << "}]";
			} else if (value.isLogicalEdge()) {
				const auto &edge = value.asLogicalEdge();
				out << "[\"edge\",{\"internal_id\":" << edge.edge_id << ",\"source_id\":" << edge.source_node_id
					<< ",\"target_id\":" << edge.target_node_id << ",\"type\":" << quoted(edge.type) << ",\"properties\":";
				scalar(out, PropertyValue(edge.properties)); out << "}]";
			} else throw std::runtime_error("unsupported observer entity");
		}
		out << ']';
	}
	out << "]}"; return out.str();
}
std::vector<std::string> fields(const std::string &line) {
	std::vector<std::string> result; size_t begin = 0U;
	while (true) { const auto end = line.find('\t', begin); result.push_back(line.substr(begin, end - begin)); if (end == std::string::npos) break; begin = end + 1U; }
	return result;
}
}
int main(int argc, char **argv) {
	try {
		if (argc != 2 || std::filesystem::exists(argv[1])) throw std::invalid_argument("worker requires a fresh private database path");
		RelocatableStorageBackendOptions options; options.data_path = argv[1]; options.open_mode = OpenMode::OPEN_CREATE_NEW_FILE;
		auto backend = RelocatableStorageBackend::initializeNew(options); backend->open();
		LogicalReadExecutionOptions limits;
		auto engine = std::make_unique<QueryEngine>(backend, backend, limits);
		std::map<std::string, std::unique_ptr<LogicalGraphTransaction>> transactions;
		std::string line;
		while (std::getline(std::cin, line)) {
			try {
				const auto f = fields(line); const auto &command = f.at(0);
				std::string response = "{\"ok\":true}";
				if (command == "QUERY" || command == "PARAM" || command == "TXQUERY" || command == "CANCEL") {
					const bool transactionQuery = command == "TXQUERY";
					const auto &query = f.at(transactionQuery ? 2U : 1U); QueryContext context;
					if (command == "PARAM") {
						context.parameters.emplace(f.at(2), f.at(3) == "integer" ? PropertyValue(std::stoll(f.at(4))) : PropertyValue(f.at(4)));
					}
					QueryGuard cancelled(1, 0);
					if (command == "CANCEL") { std::this_thread::sleep_for(std::chrono::milliseconds(2)); context.guard = &cancelled; }
					if (transactionQuery) {
						auto &transaction = *transactions.at(f.at(1)); LogicalGraphTransactionOverlay overlay(transaction);
						context.execMode = transaction.isReadOnly() ? ExecMode::EM_READ_ONLY : ExecMode::EM_READ_WRITE;
						response = rows(engine->executePlanWithLogicalTransactionOverlay(engine->buildPlan(query), context, overlay, overlay));
					} else response = rows(engine->execute(query, context));
				} else if (command == "MIXED_THREADS") {
					const size_t operations = std::stoull(f.at(1));
					if (operations == 0U || operations > 100000U || !transactions.empty()) throw std::invalid_argument("bounded independent mixed clients required");
					std::atomic<size_t> ready{0U}, retries{0U}; std::atomic<bool> start{false};
					std::array<std::exception_ptr, 4U> failures{};
					std::vector<std::jthread> clients;
					struct StartGuard { std::atomic<bool> &flag; ~StartGuard() { flag.store(true); } } startGuard{start};
					for (size_t client = 0U; client < 4U; ++client) clients.emplace_back([&, client] {
						ready.fetch_add(1U); while (!start.load()) std::this_thread::yield();
						try {
							QueryEngine localEngine(backend, backend, limits);
							const std::string match = "MATCH (n:User {id:" + std::to_string(client) + "}) ";
							auto current = localEngine.execute(match + "RETURN n.score AS score");
							int64_t expected = std::get<int64_t>(current.getRows().at(0).at("score").asPrimitive().getVariant());
							for (size_t operation = 0U; operation < operations; ++operation) {
								if (operation % 10U != 0U) {
									const auto result = localEngine.execute(match + "RETURN n.score AS score");
									if (std::get<int64_t>(result.getRows().at(0).at("score").asPrimitive().getVariant()) != expected)
										throw std::runtime_error("mixed client lost its committed state");
									continue;
								}
								bool committed = false;
								for (size_t attempt = 0U; attempt < 1024U && !committed; ++attempt) {
									auto transaction = backend->beginLogicalGraphTransaction(false); LogicalGraphTransactionOverlay overlay(*transaction);
									QueryContext context; context.execMode = ExecMode::EM_READ_WRITE;
									(void) localEngine.executePlanWithLogicalTransactionOverlay(localEngine.buildPlan(match + "SET n.score = n.score + 1 RETURN n.score AS score"), context, overlay, overlay);
									try { (void) transaction->commit(); committed = true; ++expected; }
									catch (const LogicalGraphMutationConflict &) { retries.fetch_add(1U); }
								}
								if (!committed) throw std::runtime_error("mixed retry limit exhausted");
							}
						} catch (...) { failures[client] = std::current_exception(); }
					});
					while (ready.load() != 4U) std::this_thread::yield(); start.store(true);
					for (auto &client : clients) client.join();
					for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);
					response = "{\"ok\":true,\"retries\":" + std::to_string(retries.load()) + ",\"clients\":4,\"operations\":" + std::to_string(4U * operations) + '}';
				} else if (command == "BEGIN") {
					if (transactions.contains(f.at(1))) throw std::invalid_argument("duplicate transaction name");
					transactions.emplace(f.at(1), backend->beginLogicalGraphTransaction(f.at(2) == "read"));
				} else if (command == "COMMIT" || command == "ROLLBACK") {
					auto &transaction = *transactions.at(f.at(1));
					if (command == "COMMIT") (void) transaction.commit(); else transaction.rollback();
					transactions.erase(f.at(1));
				} else if (command == "RELEASE") transactions.erase(f.at(1));
				else if (command == "REOPEN") {
					if (!transactions.empty()) throw std::logic_error("release transactions before reopen");
					engine.reset(); backend->close(); backend->open(); engine = std::make_unique<QueryEngine>(backend, backend, limits);
				} else if (command == "READONLY" || command == "READWRITE") {
					if (!transactions.empty()) throw std::logic_error("release transactions before changing handle mode");
					engine.reset(); backend->close();
					auto reopenOptions = backend->options();
					reopenOptions.open_mode = command == "READONLY" ? OpenMode::OPEN_READ_ONLY_EXISTING_FILE : OpenMode::OPEN_EXISTING_FILE;
					backend = std::make_shared<RelocatableStorageBackend>(reopenOptions); backend->open();
					if (command == "READONLY") engine = std::make_unique<QueryEngine>(backend, limits);
					else engine = std::make_unique<QueryEngine>(backend, backend, limits);
				} else if (command == "CLEAR_CACHE") engine->getPlanCache().clear();
				else if (command == "LIMIT") {
					limits.bounds.maximum_materialized_bytes = std::stoull(f.at(1));
					if (limits.bounds.maximum_materialized_bytes == 0U) throw std::invalid_argument("zero query budget");
					engine = std::make_unique<QueryEngine>(backend, backend, limits);
				} else if (command == "INTEGRITY") (void) backend->verifyIntegrity();
				else if (command == "STATS") {
					const auto stats = backend->ioStats().data;
					response = "{\"ok\":true,\"read_bytes\":" + std::to_string(stats.read_bytes) + ",\"write_bytes\":" + std::to_string(stats.write_bytes)
						+ ",\"file_bytes\":" + std::to_string(std::filesystem::file_size(argv[1])) + ",\"cache_hits\":" + std::to_string(engine->getPlanCache().hits())
						+ ",\"rss_bytes\":" + std::to_string(QueryGuard::getCurrentRSSBytes()) + '}';
				} else if (command == "FORMAT") {
					response = "{\"ok\":true,\"format_envelope\":" + std::to_string(RSS_FORMAT_VERSION) + ",\"superblock\":" + std::to_string(RSS_SUPERBLOCK_VERSION) + '}';
				} else if (command == "MAINTAIN") {
					LargeValueReachabilityReclaimOptions reclaim;
					reclaim.maximum_manifest_locations = 262144U; reclaim.maximum_manifest_metadata_pages = 1048576U;
					reclaim.maximum_manifest_nodes = 262144U; reclaim.maximum_owner_locations = 1048576U; reclaim.maximum_owner_metadata_pages = 1048576U;
					reclaim.maximum_data_pages = 2097152U; reclaim.maximum_retirements = 65536U; reclaim.maximum_page_locations = 2097152U; reclaim.maximum_pools = 65536U;
					reclaim.first_index_slot = PhysicalSlotId(1U); reclaim.last_index_slot = PhysicalSlotId(999999U);
					reclaim.first_journal_state_slot = PhysicalSlotId(1000000U); reclaim.last_journal_state_slot = PhysicalSlotId(1000015U);
					reclaim.journal_state_page_owner_id = 0x4C564D4AU; reclaim.journal_state_id = 0x4C564D4AU;
					const auto result = backend->reclaimUnreachableLargeValues(reclaim);
					response = "{\"ok\":true,\"retired_values\":" + std::to_string(result.retired_values) + '}';
				} else throw std::invalid_argument("unknown worker command");
				std::cout << response << std::endl;
			} catch (const std::exception &error) { std::cout << "{\"ok\":false,\"error\":" << quoted(error.what()) << '}' << std::endl; }
		}
		transactions.clear(); engine.reset(); backend->close(); return 0;
	} catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
