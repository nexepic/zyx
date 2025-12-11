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
        // Grammar: MATCH pattern (WHERE ...)?
        // pattern -> patternPart -> ...

        auto parts = ctx->pattern()->patternPart();

        // Handle case with no pattern (though syntax error usually prevents this)
        if (parts.empty()) return std::any();

        // 1. Process the First Path
        // We focus on the Head Node of the first pattern part for the Scan.
        auto part = parts[0];
        auto headNodePat = part->nodePattern();

        // Access members directly as per your G4 structure
        std::string var = (headNodePat->variable) ? headNodePat->variable->getText() : "";
        std::string label = (headNodePat->label) ? headNodePat->label->getText() : "";

        // --- OPTIMIZER: Index Pushdown Logic ---
        // We look for inline properties: MATCH (n {age: 25, active: true})
        // If present, we push the FIRST property down to the Scan operator
        // to utilize a potential Property Index.

        std::string pushdownKey = "";
        PropertyValue pushdownValue;
        bool hasPushdown = false;

        // This vector holds filters that couldn't be pushed down (indexes 1..N)
        std::vector<std::pair<std::string, PropertyValue>> remainingFilters;

        if (headNodePat->mapLiteral()) {
            auto entries = headNodePat->mapLiteral()->mapEntry();
            for (size_t i = 0; i < entries.size(); ++i) {
                std::string key = entries[i]->key->getText();
                auto val = parseValue(entries[i]->literal());

                if (i == 0) {
                    // Push the first property to the Scan
                    pushdownKey = key;
                    pushdownValue = val;
                    hasPushdown = true;
                } else {
                    // Keep subsequent properties as standard Filters
                    remainingFilters.emplace_back(key, val);
                }
            }
        }

        // 2. Factory: Create Optimized SCAN Operator
        // Pass the pushdown key/value to the planner.
        // The NodeScanOperator will decide if it can use an index.
        auto currentOp = planner_->scan(var, label, pushdownKey, pushdownValue);

        // 3. Factory: Create FILTER Operators for remaining properties
        for (const auto& [key, val] : remainingFilters) {
            auto predicate = [var, key, val](const query::execution::Record& r) {
                auto n = r.getNode(var);
                if (!n) return false;
                const auto& props = n->getProperties();
                auto it = props.find(key);
                return it != props.end() && it->second == val;
            };

            // Wrap current op in a Filter
            currentOp = planner_->filter(std::move(currentOp), predicate);
        }

        // Update the global root
        rootOp_ = std::move(currentOp);

        // --- TODO: Traversal Handling ---
        // Check for (a)-[r]->(b) chain
        auto chains = part->patternElementChain();
        if (!chains.empty()) {
            // Future implementation:
            // Iterate chains and wrap rootOp_ in TraversalOperator(s)
        }

        // --- TODO: WHERE Clause Handling ---
        // Parse ctx->whereClause() and wrap rootOp_ in generic FilterOperator
        // if (ctx->whereClause()) { ... }

        return std::any();
    }

	// --- CREATE statement processing ---
	std::any CypherToPlanVisitor::visitCreateStatement(CypherParser::CreateStatementContext *ctx) {
		// Grammar: CREATE ( indexDefinition | pattern )

		if (ctx->indexDefinition()) {
			auto idxDef = ctx->indexDefinition();

			// Extract Label and Property from grammar
			// Grammar: K_INDEX K_ON ':' label=ID '(' property=ID ')'

			// Note: Since we use 'label=ID' in G4, we access via the label token.
			std::string label = idxDef->label->getText();
			std::string property = idxDef->property->getText();

			// Create the operator
			auto op = planner_->createIndex(label, property);

			// Set as root (DDL is usually a standalone operation)
			chainOperator(std::move(op));

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
