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
#include "graph/query/execution/operators/CreateNodeOperator.hpp"
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

		if (auto *nodeOp = dynamic_cast<query::execution::operators::CreateNodeOperator *>(newOp.get())) {
			nodeOp->setChild(std::move(rootOp_));
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
		// pattern -> patternPart (',' patternPart)*

		// 1. Get Pattern Parts
		auto parts = ctx->pattern()->patternPart();
		if (parts.empty())
			return std::any();

		// NOTE: Currently we handle the first path.
		// Supporting multiple comma-separated paths requires a CartesianProduct operator.
		auto part = parts[0];

		// =========================================================
		// PART 1: HEAD NODE SCAN
		// Pattern: (n:Label {prop: val})
		// =========================================================

		auto headNodePat = part->nodePattern(); // anonymousPatternPart -> patternElement -> nodePattern

		// Extract Metadata (Direct access to Token pointers based on your Grammar)
		std::string currentVar = (headNodePat->variable) ? headNodePat->variable->getText() : "";
		std::string headLabel = (headNodePat->label) ? headNodePat->label->getText() : "";

		// --- OPTIMIZER: Index Pushdown Logic ---
		// Look for inline properties: (n {name: 'Alice', age: 30})
		// We push the FIRST property down to the Scan operator to utilize potential indexes.

		std::string pushdownKey = "";
		PropertyValue pushdownValue;
		bool hasPushdown = false;

		// Hold remaining properties that need explicit filtering (indices 1..N)
		std::vector<std::pair<std::string, PropertyValue>> remainingHeadFilters;

		if (headNodePat->mapLiteral()) {
			auto entries = headNodePat->mapLiteral()->mapEntry();
			for (size_t i = 0; i < entries.size(); ++i) {
				std::string key = entries[i]->key->getText();
				auto val = parseValue(entries[i]->literal());

				if (i == 0) {
					// Optimization: Pass to Scan
					pushdownKey = key;
					pushdownValue = val;
					hasPushdown = true;
				} else {
					// Defer to Filter
					remainingHeadFilters.emplace_back(key, val);
				}
			}
		}

		// Factory: Create Optimized SCAN Operator
		auto currentOp = planner_->scan(currentVar, headLabel, pushdownKey, pushdownValue);

		// Factory: Create FILTER Operators for remaining head properties
		for (const auto &[key, val]: remainingHeadFilters) {
			auto predicate = [currentVar, key, val](const query::execution::Record &r) {
				auto n = r.getNode(currentVar);
				// Note: Node properties must be hydrated by Scan or retrieved here
				if (!n)
					return false;
				const auto &props = n->getProperties();
				auto it = props.find(key);
				return it != props.end() && it->second == val;
			};

			std::string desc = currentVar + "." + key + " == " + val.toString();

			currentOp = planner_->filter(std::move(currentOp), predicate, desc);
		}

		// Set the initial root
		rootOp_ = std::move(currentOp);

		// =========================================================
		// PART 2: TRAVERSAL CHAIN
		// Pattern: -[r:Type]->(m)
		// =========================================================

		auto chains = part->patternElementChain();
		for (auto chain: chains) {
			auto relPat = chain->relationshipPattern();
			auto nodePat = chain->nodePattern();

			// --- A. Parse Relationship Direction ---
			// Grammar: (LT? '-') relationshipDetail ('-' GT?)
			bool hasLeftArrow = (relPat->LT() != nullptr);
			bool hasRightArrow = (relPat->GT() != nullptr);

			std::string direction = "both";
			if (hasLeftArrow && !hasRightArrow) {
				direction = "in"; // <-[]-
			} else if (!hasLeftArrow && hasRightArrow) {
				direction = "out"; // -[]->
			}

			// --- B. Parse Relationship Details ---
			// Grammar: '[' (variable)? (':' label)? (mapLiteral)? ']'
			auto relDetail = relPat->relationshipDetail();

			std::string edgeVar = (relDetail->variable) ? relDetail->variable->getText() : "";
			std::string edgeLabel = (relDetail->label) ? relDetail->label->getText() : "";

			// --- C. Parse Target Node ---
			std::string targetVar = (nodePat->variable) ? nodePat->variable->getText() : "";
			std::string targetLabel = (nodePat->label) ? nodePat->label->getText() : "";

			// --- D. Build TRAVERSAL Operator ---
			// This expands the current result set by joining edges and target nodes
			rootOp_ = planner_->traverse(std::move(rootOp_), // Child
										 currentVar, // Source (Variable from previous step)
										 edgeVar, // Edge Variable
										 targetVar, // Target Variable
										 edgeLabel, // Edge Type Filter
										 direction // Direction
			);

			// --- E. Apply Filters to Target Node ---

			// 1. Filter Target Label (if specified)
			if (!targetLabel.empty()) {
				auto labelPred = [targetVar, targetLabel](const query::execution::Record &r) {
					auto n = r.getNode(targetVar);
					return n && n->getLabel() == targetLabel;
				};
				std::string desc = "Label(" + targetVar + ") == " + targetLabel;
				rootOp_ = planner_->filter(std::move(rootOp_), labelPred, desc);
			}

			// 2. Filter Target Properties (Inline map)
			if (nodePat->mapLiteral()) {
				auto entries = nodePat->mapLiteral()->mapEntry();
				for (auto entry: entries) {
					std::string key = entry->key->getText();
					auto val = parseValue(entry->literal());

					auto propPred = [targetVar, key, val](const query::execution::Record &r) {
						auto n = r.getNode(targetVar);
						if (!n)
							return false;
						const auto &props = n->getProperties();
						auto it = props.find(key);
						return it != props.end() && it->second == val;
					};
					std::string desc = targetVar + "." + key + " == " + val.toString();
					rootOp_ = planner_->filter(std::move(rootOp_), propPred, desc);
				}
			}

			// --- F. Apply Filters to Edge ---
			if (relDetail->mapLiteral()) {
				auto entries = relDetail->mapLiteral()->mapEntry();
				for (auto entry: entries) {
					std::string key = entry->key->getText();
					auto val = parseValue(entry->literal());

					auto edgePred = [edgeVar, key, val](const query::execution::Record &r) {
						auto e = r.getEdge(edgeVar);
						if (!e)
							return false;
						// Assuming Edge object has properties hydrated or accessible
						const auto &props = e->getProperties();
						auto it = props.find(key);
						return it != props.end() && it->second == val;
					};
					std::string desc = edgeVar + "." + key + " == " + val.toString();
					rootOp_ = planner_->filter(std::move(rootOp_), edgePred, desc);
				}
			}

			// --- G. Advance Source Variable ---
			// For the next hop in the chain, the current target becomes the source
			currentVar = targetVar;
		}

		// =========================================================
		// PART 3: EXTERNAL WHERE CLAUSE
		// Pattern: MATCH ... WHERE n.age > 10
		// =========================================================

		if (ctx->whereClause()) {
			auto whereCtx = ctx->whereClause();

			// Simple handling for: WHERE n.prop = value
			// Note: A full implementation requires a recursive ExpressionVisitor.
			// Here we provide a basic implementation for equality checks to demonstrate integration.

			std::string varName = whereCtx->propertyExpression()->ID(0)->getText();
			std::string propName = whereCtx->propertyExpression()->ID(1)->getText();
			auto literalVal = parseValue(whereCtx->literal());
			std::string op = whereCtx->op->getText();

			auto wherePred = [varName, propName, literalVal, op](const query::execution::Record &r) {
				auto n = r.getNode(varName);
				if (!n)
					return false;

				const auto &props = n->getProperties();
				auto it = props.find(propName);
				if (it == props.end())
					return false;

				// Simple operator handling
				if (op == "=")
					return it->second == literalVal;
				if (op == "<>")
					return it->second != literalVal;
				// Add > < etc. if PropertyValue supports comparison operators

				return false;
			};

			std::string desc = varName + "." + propName + " " + op + " " + literalVal.toString();
			rootOp_ = planner_->filter(std::move(rootOp_), wherePred, desc);
		}

		if (ctx->returnClause()) {
			auto returnBody = ctx->returnClause()->returnBody();

			// Handle "RETURN *" (Pass through everything)
			if (returnBody->getText() == "*") {
				// If "*" is used, we usually don't add a ProjectOperator,
				// implicitly allowing all variables in the Record to pass through.
			} else {
				std::vector<std::string> projectionVars;
				auto items = returnBody->returnItem();

				for (auto item : items) {
					// Extract variable name from expression
					// Note: This is a simplification. A robust implementation would visit the expression tree.
					// Assuming expression is a simple ID for now.
					std::string varName = item->expression()->getText();

					// Handle Alias: RETURN n AS node
					// For now, we project the source variable name.
					// If aliasing is required, ProjectOperator needs to support renaming.

					projectionVars.push_back(varName);
				}

				// Add ProjectOperator to the top of the pipeline
				if (!projectionVars.empty()) {
					rootOp_ = planner_->project(std::move(rootOp_), projectionVars);
				}
			}
		}

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
