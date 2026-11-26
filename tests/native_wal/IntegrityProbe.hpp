#pragma once
#include <iostream>
#include <future>
#include <dlfcn.h>
#include <unistd.h>
#include "graph/storage/LogicalGraphWrite.hpp"
#include "graph/storage/NativeWalDatabase.hpp"
#include "graph/query/execution/ResourceCoordinator.hpp"
#include "graph/storage/StorageBackendFactory.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

namespace integrity_test {
	template<class Backend>
	void interleave(Backend &backend, const graph::storage::NativeWalIntegrityBackend &audit) {
		using namespace graph::storage;
		const auto before = audit.verifyIntegrity();
		if (!before.wal_transactions) throw std::runtime_error("Expected an active confirmed WAL");
		auto &writer = dynamic_cast<LogicalGraphTransactionalBackend &>(backend);
		auto staged = [&](const char *label) {
			auto tx = writer.beginLogicalGraphTransaction(false);
			LogicalGraphMutationBatch batch;
			batch.node_creates.push_back({{label}, {}});
			(void)tx->stageMutationBatch(std::move(batch));
			return tx;
		};
		auto privateTx = staged("PrivateEdit");
		auto publicTx = staged("ConfirmedEdit");
		auto arm = reinterpret_cast<void(*)(int, int)>(dlsym(RTLD_DEFAULT, "zyxArmWalAudit"));
		int signal[2], release[2];
		if (!arm || pipe(signal) || pipe(release)) throw std::runtime_error("Missing audit barrier");
		auto checking = std::async(std::launch::async, [&] {
			arm(signal[1], release[0]);
			return audit.verifyIntegrity();
		});
		char token = '1';
		if (read(signal[0], &token, 1) != 1) throw std::runtime_error("Audit did not reach WAL");
		std::promise<void> entering;
		auto committing = std::async(std::launch::async, [&] { entering.set_value(); return publicTx->commit(); });
		entering.get_future().wait();
		std::cout << "paused" << std::endl;
		std::string line;
		if (!std::getline(std::cin, line) || line != "release") _exit(82);
		if (write(release[1], &token, 1) != 1) _exit(83);
		auto selected = checking.get();
		(void)committing.get();
		auto latest = audit.verifyIntegrity();
		if (selected.generation != before.generation || selected.nodes != before.nodes ||
			selected.confirmed_wal_bytes != before.confirmed_wal_bytes ||
			latest.generation != before.generation + 1 || latest.nodes != before.nodes + 1)
			throw std::runtime_error("A verifier mixed confirmations or private mutations");
		auto fresh = dynamic_cast<LogicalGraphReadBackend &>(backend).openLogicalGraphReadSnapshot();
		size_t published = 0, hidden = 0;
		fresh->visitNodes(latest.nodes + 1, [&](const LogicalGraphNode &n) {
			published += std::count(n.labels.begin(), n.labels.end(), "ConfirmedEdit");
			hidden += std::count(n.labels.begin(), n.labels.end(), "PrivateEdit");
			return true;
		});
		if (published != 1 || hidden) throw std::runtime_error("Incorrect committed row identity");
		for (auto fd : {signal[0], signal[1], release[0], release[1]}) close(fd);
		std::cout << "serialized" << std::endl;
	}
	inline int inspect(const std::string &path, bool writable, size_t budget) {
		using namespace graph::storage;
		auto s = StorageBackendFactory::create(path, budget, writable ? OpenMode::OPEN_EXISTING_FILE
																	 : OpenMode::OPEN_READ_ONLY_EXISTING_FILE);
		const auto &audit = dynamic_cast<const NativeWalIntegrityBackend &>(*s.backend);
		try {
			(void)audit.verifyIntegrity();
			throw std::runtime_error("Closed audit succeeded");
		} catch (const std::logic_error &) {}
		s.backend->open();
		std::cout << "ready" << std::endl;
		std::string command;
		while (std::getline(std::cin, command)) {
			try {
				if (command == "exhaust") {
					using namespace graph::query::execution::detail;
					auto domain = dynamic_cast<LogicalGraphReadBackend &>(*s.backend).queryResourceDomain();
					const auto kind = ResourceClass::RCK_MAINTENANCE;
					ResourceAccount account(domain, kind, domain->classMaximum(kind));
					ResourceLease lease(account);
					lease.set(account.available());
					try {
						(void)audit.verifyIntegrity();
						throw std::runtime_error("Verifier bypassed occupied maintenance quota");
					} catch (const ResourceAdmissionDenied &) {}
					lease.set(0);
					const auto used = domain->usage().total;
					(void)audit.verifyIntegrity();
					if (domain->usage().total != used) throw std::runtime_error("Leaked maintenance grant");
					std::cout << "admitted after release" << std::endl;
					continue;
				}
				if (command == "interleave") { interleave(*s.backend, audit); continue; }
				if (command == "pin-stage-commit") {
					auto &reader = dynamic_cast<LogicalGraphReadBackend &>(*s.backend);
					auto old = reader.openLogicalGraphReadSnapshot();
					const auto before = audit.verifyIntegrity();
					auto &writer = dynamic_cast<LogicalGraphTransactionalBackend &>(*s.backend);
					auto tx = writer.beginLogicalGraphTransaction(false);
					LogicalGraphMutationBatch batch;
					batch.node_creates.push_back({{"AuditOnly"}, {}});
					(void)tx->stageMutationBatch(std::move(batch));
					if (audit.verifyIntegrity().nodes != before.nodes)
						throw std::runtime_error("Staged row leaked into confirmed verification");
					(void)tx->commit();
					auto committed = audit.verifyIntegrity();
					if (committed.nodes != before.nodes + 1 || committed.generation != before.generation + 1)
						throw std::runtime_error("Verifier missed acknowledged transaction");
					s.backend->flush();
					size_t count = 0;
					old->visitNodes(before.nodes + 2, [&](const LogicalGraphNode &) { ++count; return true; });
					if (count != before.nodes || audit.verifyIntegrity().nodes != committed.nodes)
						throw std::runtime_error("Pinned view or checkpoint verification changed");
					std::cout << "pinned" << std::endl;
					continue;
				}
				const auto r = audit.verifyIntegrity();
				std::cout << "ok " << r.generation << " " << r.base_generation << " " << r.embedded_transactions
						  << " " << r.wal_transactions << " " << r.confirmed_wal_bytes << " " << r.nodes << " "
						  << r.edges << " " << r.unique_constraints << " " << r.has_declared_column << " "
						  << r.base_column_complete << std::endl;
			} catch (const FormatException &e) {
				std::cout << "invalid " << e.what() << std::endl;
			} catch (const graph::query::execution::detail::ResourceAdmissionDenied &e) {
				std::cout << "budget " << e.what() << std::endl;
			}
		}
		// Avoid checkpoint-on-clean-close: only the audit may affect the fixture.
		_Exit(0);
	}
}
