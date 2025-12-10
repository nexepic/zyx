/**
 * @file CypherToPlanVisitor.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/9
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "CypherToPlanVisitor.hpp"
#include "graph/query/execution/operators/CreateEdgeOperator.hpp"
#include "graph/query/execution/operators/FilterOperator.hpp"

namespace graph::parser::cypher {

	CypherToPlanVisitor::CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner) :
		planner_(std::move(planner)) {}

	std::unique_ptr<query::execution::PhysicalOperator> CypherToPlanVisitor::getPlan() { return std::move(rootOp_); }

	std::any CypherToPlanVisitor::visitQuery(CypherParser::QueryContext *ctx) { return visitChildren(ctx); }

	// --- Helper function: Chain operators (Pipeline) ---
	void CypherToPlanVisitor::chainOperator(std::unique_ptr<query::execution::PhysicalOperator> newOp) {
		if (!rootOp_) {
			rootOp_ = std::move(newOp);
			return;
		}

		// Try to chain the new operator to the end of the existing tree
		// Note: CreateEdgeOperator requires an upstream data flow, so we set rootOp_ as its child
		if (auto *edgeOp = dynamic_cast<query::execution::operators::CreateEdgeOperator *>(newOp.get())) {
			edgeOp->setChild(std::move(rootOp_));
			rootOp_ = std::move(newOp);
			return;
		}

		// For possible "Sink" or "PassThrough" operators like CreateNode, if there is no explicit chaining interface,
		// in this simple implementation, we temporarily replace rootOp_.
		// In a more complex implementation, a CartesianProductOperator or SequenceOperator should be used.
		// Here we assume Sequential Execution.
		rootOp_ = std::move(newOp);
	}

	// --- MATCH statement processing ---
	std::any CypherToPlanVisitor::visitMatchStatement(CypherParser::MatchStatementContext *ctx) {
		// Grammar: MATCH pattern
		// pattern -> patternPart*

		auto parts = ctx->pattern()->patternPart();

		// Simple implementation: currently only handles the first path
		// A complete implementation needs to handle multiple paths (Cartesian Product)
		if (parts.empty())
			return std::any();

		auto part = parts[0];
		auto headNodePat = part->nodePattern();

		// 1. Process the head node SCAN
		// Use ->variable instead of ->variable() because we used variable=ID in g4
		std::string var = (headNodePat->variable) ? headNodePat->variable->getText() : "";
		std::string label = (headNodePat->label) ? headNodePat->label->getText() : "";

		// Factory method to create Scan
		auto currentOp = planner_->scan(var, label);

		// 2. Process inline property filtering for the head node {key:val}
		if (headNodePat->mapLiteral()) {
			auto entries = headNodePat->mapLiteral()->mapEntry();
			for (auto entry: entries) {
				std::string key = entry->key->getText();
				auto val = parseValue(entry->literal());

				auto predicate = [var, key, val](const query::execution::Record &r) {
					auto n = r.getNode(var);
					if (!n)
						return false;
					const auto &props = n->getProperties();
					auto it = props.find(key);
					return it != props.end() && it->second == val;
				};

				currentOp = planner_->filter(std::move(currentOp), predicate);
			}
		}

		// Update the root operator
		rootOp_ = std::move(currentOp);

		// TODO: Handle chained MATCH, e.g., (a)-[r]->(b)
		// This requires support from the planner_->traverse() interface
		auto chains = part->patternElementChain();
		if (!chains.empty()) {
			// If there's a chain, a TraversalOperator should be generated here
			// Since we are currently focused on Create, this is left blank or could throw a not-implemented exception
		}

		// TODO: Handle WHERE clause
		// if (ctx->whereClause()) ...

		return std::any();
	}

	// --- CREATE statement processing ---
	std::any CypherToPlanVisitor::visitCreateStatement(CypherParser::CreateStatementContext *ctx) {
		// Grammar: CREATE ( indexDefinition | pattern )

		if (ctx->indexDefinition()) {
			// Skip DDL operations for now
			return std::any();
		}

		auto pattern = ctx->pattern();
		if (!pattern)
			return std::any();

		// Iterate over all comma-separated paths: CREATE (a), (b)
		for (auto part: pattern->patternPart()) {

			// --- 1. Process the head node of the path ---
			auto headNodePat = part->nodePattern();

			std::string headVar = (headNodePat->variable) ? headNodePat->variable->getText() : "";
			std::string headLabel =
					(headNodePat->label) ? headNodePat->label->getText() : ""; // CREATE usually requires a label

			// Extract properties
			std::unordered_map<std::string, PropertyValue> headProps;
			if (headNodePat->mapLiteral()) {
				auto entries = headNodePat->mapLiteral()->mapEntry();
				for (auto entry: entries) {
					headProps.emplace(entry->key->getText(), parseValue(entry->literal()));
				}
			}

			// Call the planner to create the node operator
			auto headOp = planner_->create(headVar, headLabel, headProps);
			chainOperator(std::move(headOp));

			// Record the current node variable to be the source for the next edge
			std::string previousNodeVar = headVar;

			// --- 2. Process the path chain: -[r]->(m) ---
			for (auto chain: part->patternElementChain()) {
				auto relPat = chain->relationshipPattern();
				auto targetNodePat = chain->nodePattern();

				// A. Prepare target node data
				std::string targetVar = (targetNodePat->variable) ? targetNodePat->variable->getText() : "";
				std::string targetLabel = (targetNodePat->label) ? targetNodePat->label->getText() : "";

				std::unordered_map<std::string, PropertyValue> targetProps;
				if (targetNodePat->mapLiteral()) {
					auto entries = targetNodePat->mapLiteral()->mapEntry();
					for (auto entry: entries) {
						targetProps.emplace(entry->key->getText(), parseValue(entry->literal()));
					}
				}

				// -> Create the target node operator
				auto targetOp = planner_->create(targetVar, targetLabel, targetProps);
				chainOperator(std::move(targetOp));

				// B. Prepare relationship data
				auto relDetail = relPat->relationshipDetail();
				std::string edgeVar = (relDetail->variable) ? relDetail->variable->getText() : "";
				std::string edgeLabel = (relDetail->label) ? relDetail->label->getText() : "";

				std::unordered_map<std::string, PropertyValue> edgeProps;
				if (relDetail->mapLiteral()) {
					auto entries = relDetail->mapLiteral()->mapEntry();
					for (auto entry: entries) {
						edgeProps.emplace(entry->key->getText(), parseValue(entry->literal()));
					}
				}

				// -> Create the relationship operator (connecting previousNodeVar -> targetVar)
				auto edgeOp = planner_->create(edgeVar, edgeLabel, edgeProps, previousNodeVar, targetVar);
				chainOperator(std::move(edgeOp));

				// Advance: the current target node becomes the source for the next edge
				previousNodeVar = targetVar;
			}
		}

		return std::any();
	}

	std::any CypherToPlanVisitor::visitLiteral(CypherParser::LiteralContext *ctx) { return parseValue(ctx); }

	graph::PropertyValue CypherToPlanVisitor::parseValue(CypherParser::LiteralContext *ctx) {
		if (ctx->STRING()) {
			std::string s = ctx->STRING()->getText();
			// Remove quotes from the string
			if (s.length() >= 2)
				return PropertyValue(s.substr(1, s.length() - 2));
			return PropertyValue(s);
		}
		if (ctx->INTEGER())
			return PropertyValue(std::stoll(ctx->INTEGER()->getText()));
		if (ctx->DECIMAL())
			return PropertyValue(std::stod(ctx->DECIMAL()->getText()));
		if (ctx->BOOLEAN()) {
			std::string val = ctx->BOOLEAN()->getText();
			return PropertyValue(val == "TRUE" || val == "true");
		}
		if (ctx->NULL_LITERAL())
			return PropertyValue();

		return PropertyValue();
	}

} // namespace graph::parser::cypher
