/**
 * @file NativeProcedureManifest.hpp
 * @brief Single source of truth for procedures implemented by the native logical runtime.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <array>
#include <string_view>

namespace graph::query {

	// Keep procedure metadata independent from the parser and physical operator
	// trees so the public procedure surface has one native ownership boundary.
	enum class NativeProcedureKind {
		NPK_SHORTEST_PATH,
		NPK_VECTOR_QUERY_NODES,
		NPK_VECTOR_TRAIN,
		NPK_GDS_GRAPH_PROJECT,
		NPK_GDS_GRAPH_DROP,
		NPK_GDS_GRAPH_DROP_ALL,
		NPK_GDS_GRAPH_EXISTS,
		NPK_GDS_GRAPH_LIST,
		NPK_GDS_GRAPH_SCHEMA,
		NPK_GDS_PAGE_RANK,
		NPK_GDS_WCC,
		NPK_GDS_BETWEENNESS,
		NPK_GDS_CLOSENESS,
		NPK_GDS_LEIDEN,
		NPK_GDS_DIJKSTRA,
		NPK_DBMS_LIST_CONFIG,
		NPK_DBMS_GET_CONFIG,
		NPK_DBMS_SET_CONFIG,
		NPK_DBMS_SHOW_STATS,
		NPK_DBMS_RESET_STATS,
		NPK_DBMS_LIST_SLOW_QUERIES,
		NPK_DBMS_CLEAR_SLOW_QUERIES,
	};

	struct NativeProcedureManifestEntry {
		std::string_view name;
		NativeProcedureKind kind;
		bool mutates_data = false;
		bool mutates_schema = false;
	};

	// Explicit transactions may execute only procedures whose complete read set is
	// available through LogicalGraphTransactionOverlay. GDS projections need a
	// transaction-owned catalog so rollback can discard their process-local
	// derived state without mutating the Database-owned catalog. Vector search
	// is permitted only when the transaction owns a matching pinned derived
	// snapshot; Vector maintenance remains autocommit-only.
	enum class NativeProcedureTransactionMode {
		NPTM_OVERLAY_SNAPSHOT_READ,
		NPTM_TRANSACTION_LOCAL_CATALOG,
		NPTM_AUTOCOMMIT_ONLY,
	};

	inline constexpr auto NATIVE_PROCEDURE_MANIFEST = std::to_array<NativeProcedureManifestEntry>({
			{"algo.shortestPath", NativeProcedureKind::NPK_SHORTEST_PATH, false, false},
			{"db.index.vector.queryNodes", NativeProcedureKind::NPK_VECTOR_QUERY_NODES, false, false},
			{"db.index.vector.train", NativeProcedureKind::NPK_VECTOR_TRAIN, true, false},
			// These mutate only the QueryEngine-owned, process-local projection catalog.
			// They never publish graph data or schema changes to the database file.
			{"gds.graph.project", NativeProcedureKind::NPK_GDS_GRAPH_PROJECT, false, false},
			{"gds.graph.drop", NativeProcedureKind::NPK_GDS_GRAPH_DROP, false, false},
			{"gds.graph.dropAll", NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL, false, false},
			{"gds.graph.exists", NativeProcedureKind::NPK_GDS_GRAPH_EXISTS, false, false},
			{"gds.graph.list", NativeProcedureKind::NPK_GDS_GRAPH_LIST, false, false},
			{"gds.graph.schema", NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA, false, false},
			{"gds.pageRank.stream", NativeProcedureKind::NPK_GDS_PAGE_RANK, false, false},
			{"gds.wcc.stream", NativeProcedureKind::NPK_GDS_WCC, false, false},
			{"gds.betweenness.stream", NativeProcedureKind::NPK_GDS_BETWEENNESS, false, false},
			{"gds.closeness.stream", NativeProcedureKind::NPK_GDS_CLOSENESS, false, false},
			{"gds.leiden.stream", NativeProcedureKind::NPK_GDS_LEIDEN, false, false},
			{"gds.shortestPath.dijkstra.stream", NativeProcedureKind::NPK_GDS_DIJKSTRA, false, false},
			{"dbms.listConfig", NativeProcedureKind::NPK_DBMS_LIST_CONFIG, false, false},
			{"dbms.getConfig", NativeProcedureKind::NPK_DBMS_GET_CONFIG, false, false},
			// Durable configuration is native metadata, not logical graph data, but
			// it is a write-side side effect and must not run in a read-only context.
			{"dbms.setConfig", NativeProcedureKind::NPK_DBMS_SET_CONFIG, true, false},
			{"dbms.showStats", NativeProcedureKind::NPK_DBMS_SHOW_STATS, false, false},
			{"dbms.resetStats", NativeProcedureKind::NPK_DBMS_RESET_STATS, true, false},
			{"dbms.listSlowQueries", NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES, false, false},
			{"dbms.clearSlowQueries", NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES, true, false},
	});

	[[nodiscard]] constexpr const auto &nativeProcedureManifest() noexcept { return NATIVE_PROCEDURE_MANIFEST; }

	[[nodiscard]] constexpr const NativeProcedureManifestEntry *
	findNativeProcedureManifestEntry(const std::string_view name) noexcept {
		for (const NativeProcedureManifestEntry &entry: nativeProcedureManifest()) {
			if (entry.name == name) {
				return &entry;
			}
		}
		return nullptr;
	}

	[[nodiscard]] constexpr NativeProcedureTransactionMode
	nativeProcedureTransactionMode(const NativeProcedureKind kind) noexcept {
		switch (kind) {
			case NativeProcedureKind::NPK_SHORTEST_PATH:
			case NativeProcedureKind::NPK_VECTOR_QUERY_NODES:
				return NativeProcedureTransactionMode::NPTM_OVERLAY_SNAPSHOT_READ;
			case NativeProcedureKind::NPK_GDS_GRAPH_PROJECT:
			case NativeProcedureKind::NPK_GDS_GRAPH_DROP:
			case NativeProcedureKind::NPK_GDS_GRAPH_DROP_ALL:
			case NativeProcedureKind::NPK_GDS_GRAPH_EXISTS:
			case NativeProcedureKind::NPK_GDS_GRAPH_LIST:
			case NativeProcedureKind::NPK_GDS_GRAPH_SCHEMA:
			case NativeProcedureKind::NPK_GDS_PAGE_RANK:
			case NativeProcedureKind::NPK_GDS_WCC:
			case NativeProcedureKind::NPK_GDS_BETWEENNESS:
			case NativeProcedureKind::NPK_GDS_CLOSENESS:
			case NativeProcedureKind::NPK_GDS_LEIDEN:
			case NativeProcedureKind::NPK_GDS_DIJKSTRA:
				return NativeProcedureTransactionMode::NPTM_TRANSACTION_LOCAL_CATALOG;
			case NativeProcedureKind::NPK_VECTOR_TRAIN:
			case NativeProcedureKind::NPK_DBMS_LIST_CONFIG:
			case NativeProcedureKind::NPK_DBMS_GET_CONFIG:
			case NativeProcedureKind::NPK_DBMS_SET_CONFIG:
			case NativeProcedureKind::NPK_DBMS_SHOW_STATS:
			case NativeProcedureKind::NPK_DBMS_RESET_STATS:
			case NativeProcedureKind::NPK_DBMS_LIST_SLOW_QUERIES:
			case NativeProcedureKind::NPK_DBMS_CLEAR_SLOW_QUERIES:
				return NativeProcedureTransactionMode::NPTM_AUTOCOMMIT_ONLY;
		}
		return NativeProcedureTransactionMode::NPTM_AUTOCOMMIT_ONLY;
	}

} // namespace graph::query
