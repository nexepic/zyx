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

namespace graph::parser::cypher {

	CypherToPlanVisitor::CypherToPlanVisitor(std::shared_ptr<query::QueryPlanner> planner) :
		planner_(std::move(planner)) {}

	std::unique_ptr<query::execution::PhysicalOperator> CypherToPlanVisitor::getPlan() { return std::move(rootOp_); }

	// --- Entry ---
	std::any CypherToPlanVisitor::visitCypher(CypherParser::CypherContext *ctx) { return visitChildren(ctx); }

	std::any CypherToPlanVisitor::visitRegularQuery(CypherParser::RegularQueryContext *ctx) {
		return visitChildren(ctx);
	}

	std::any CypherToPlanVisitor::visitSingleQuery(CypherParser::SingleQueryContext *ctx) {
		// Automatically chains children (Clauses) in order
		return visitChildren(ctx);
	}

	// --- Helper: Chain ---
	void CypherToPlanVisitor::chainOperator(std::unique_ptr<query::execution::PhysicalOperator> newOp) {
		if (!rootOp_) {
			rootOp_ = std::move(newOp);
			return;
		}
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
		rootOp_ = std::move(newOp);
	}

	std::function<bool(const query::execution::Record &)>
	CypherToPlanVisitor::buildWherePredicate(CypherParser::ExpressionContext *expr, std::string &outDesc) {

		// 1. Navigate down to Comparison
		auto comparison =
				expr->orExpression()->xorExpression(0)->andExpression(0)->notExpression(0)->comparisonExpression();

		if (!comparison)
			throw std::runtime_error("Invalid WHERE expression tree");

		// Ensure Binary Operation (LHS op RHS)
		if (comparison->arithmeticExpression().size() < 2) {
			throw std::runtime_error("WHERE clause currently only supports binary comparisons (e.g., n.age > 10)");
		}

		auto lhsArith = comparison->arithmeticExpression(0);
		auto rhsArith = comparison->arithmeticExpression(1);

		// =========================================================
		// 2. Parse LHS (Left Hand Side) - Expecting: n.prop
		// Structure: unary -> atom (accessor)
		// =========================================================

		auto lhsUnary = lhsArith->unaryExpression(0);

		// Check for Atom (Variable 'n')
		if (!lhsUnary->atom() || !lhsUnary->atom()->variable()) {
			throw std::runtime_error("LHS must start with a variable (e.g., 'n.age')");
		}
		std::string varName = lhsUnary->atom()->variable()->getText();

		// Check for Accessor (Property '.age')
		if (lhsUnary->accessor().empty()) {
			throw std::runtime_error("LHS must be a property (e.g., 'n.age', not just 'n')");
		}

		// Get the first accessor
		auto firstAccessor = lhsUnary->accessor(0);

		// Verify it is a PropertyAccess (DOT name), not a ListIndex ([0])
		// We check the context type. In generated code, check if propertyKeyName exists.
		// Based on grammar: accessor -> DOT propertyKeyName
		if (firstAccessor->DOT() == nullptr) {
			throw std::runtime_error("Array indexing (e.g. n.tags[0]) not supported in WHERE yet.");
		}

		std::string propKey = firstAccessor->propertyKeyName()->getText();

		// =========================================================
		// 3. Parse RHS (Right Hand Side) - Expecting: Literal
		// Structure: unary -> atom -> literal
		// =========================================================

		auto rhsUnary = rhsArith->unaryExpression(0);
		auto rhsAtom = rhsUnary->atom();

		if (!rhsAtom || !rhsAtom->literal()) {
			throw std::runtime_error("RHS of WHERE must be a literal value (e.g., 10, 'text')");
		}
		auto literalVal = parseValue(rhsAtom->literal());

		// =========================================================
		// 4. Parse Operator
		// =========================================================
		std::string op = "==";
		if (!comparison->EQ().empty())
			op = "=";
		else if (!comparison->NEQ().empty())
			op = "<>";
		else if (!comparison->LT().empty())
			op = "<";
		else if (!comparison->GT().empty())
			op = ">";
		else if (!comparison->LTE().empty())
			op = "<=";
		else if (!comparison->GTE().empty())
			op = ">=";

		// =========================================================
		// 5. Build Result
		// =========================================================

		outDesc = varName + "." + propKey + " " + op + " " + literalVal.toString();

		return [varName, propKey, literalVal, op](const query::execution::Record &r) -> bool {
			auto n = r.getNode(varName);
			if (!n)
				return false;

			const auto &props = n->getProperties();
			auto it = props.find(propKey);
			if (it == props.end())
				return false; // Property missing

			const auto &val = it->second;

			if (op == "=")
				return val == literalVal;
			if (op == "<>")
				return val != literalVal;
			if (op == ">")
				return val > literalVal;
			if (op == "<")
				return val < literalVal;
			if (op == ">=")
				return val >= literalVal;
			if (op == "<=")
				return val <= literalVal;

			return false;
		};
	}

	// --- MATCH ---
	std::any CypherToPlanVisitor::visitMatchStatement(CypherParser::MatchStatementContext *ctx) {
		auto pattern = ctx->pattern();
		if (!pattern)
			return std::any();

		auto parts = pattern->patternPart();
		if (parts.empty())
			return std::any();

		auto part = parts[0];
		auto element = part->patternElement();

		// 1. Head Node
		auto headNodePat = element->nodePattern();

		std::string var = extractVariable(headNodePat->variable());
		std::string label = extractLabel(headNodePat->nodeLabels());
		auto props = extractProperties(headNodePat->properties());

		// Optimization
		std::string pushKey = "";
		graph::PropertyValue pushVal;
		std::vector<std::pair<std::string, graph::PropertyValue>> residualFilters;

		if (!props.empty()) {
			auto it = props.begin();
			pushKey = it->first;
			pushVal = it->second;
			for (++it; it != props.end(); ++it) {
				residualFilters.emplace_back(it->first, it->second);
			}
		}

		auto currentOp = planner_->scan(var, label, pushKey, pushVal);

		for (const auto &[key, val]: residualFilters) {
			auto predicate = [var, key, val](const query::execution::Record &r) {
				auto n = r.getNode(var);
				if (!n)
					return false;
				const auto &p = n->getProperties();
				return p.count(key) && p.at(key) == val;
			};
			std::string desc = var + "." + key + " == " + val.toString();
			currentOp = planner_->filter(std::move(currentOp), predicate, desc);
		}

		rootOp_ = std::move(currentOp);

		// 2. Traversal
		for (auto chain: element->patternElementChain()) {
			auto relPat = chain->relationshipPattern();
			auto nodePat = chain->nodePattern();

			// Direction
			std::string direction = "both";
			bool hasLeft = (relPat->LT() != nullptr);
			bool hasRight = (relPat->GT() != nullptr);
			if (hasLeft && !hasRight)
				direction = "in";
			else if (!hasLeft && hasRight)
				direction = "out";

			// Details
			auto relDetail = relPat->relationshipDetail();
			std::string edgeVar = "", edgeLabel = "";
			std::unordered_map<std::string, graph::PropertyValue> edgeProps;

			if (relDetail) {
				edgeVar = extractVariable(relDetail->variable());
				edgeLabel = extractRelType(relDetail->relationshipTypes());
				edgeProps = extractProperties(relDetail->properties());
			}

			// Target
			std::string targetVar = extractVariable(nodePat->variable());
			std::string targetLabel = extractLabel(nodePat->nodeLabels());
			auto targetProps = extractProperties(nodePat->properties());

			// Build
			rootOp_ = planner_->traverse(std::move(rootOp_), var, edgeVar, targetVar, edgeLabel, direction);

			// Filter Target
			if (!targetLabel.empty()) {
				auto labelPred = [targetVar, targetLabel](const query::execution::Record &r) {
					auto n = r.getNode(targetVar);
					return n && n->getLabel() == targetLabel;
				};
				rootOp_ = planner_->filter(std::move(rootOp_), labelPred, "Label(" + targetVar + ")=" + targetLabel);
			}

			for (const auto &[key, val]: targetProps) {
				auto pred = [targetVar, key, val](const query::execution::Record &r) {
					auto n = r.getNode(targetVar);
					return n && n->getProperties().count(key) && n->getProperties().at(key) == val;
				};
				rootOp_ = planner_->filter(std::move(rootOp_), pred, targetVar + "." + key + "=" + val.toString());
			}

			// Filter Edge
			for (const auto &[key, val]: edgeProps) {
				auto pred = [edgeVar, key, val](const query::execution::Record &r) {
					auto e = r.getEdge(edgeVar);
					return e && e->getProperties().count(key) && e->getProperties().at(key) == val;
				};
				rootOp_ = planner_->filter(std::move(rootOp_), pred, edgeVar + "." + key + "=" + val.toString());
			}

			var = targetVar;
		}

		// 3. Where
		if (ctx->where()) {
			std::string desc;
			try {
				auto predicate = buildWherePredicate(ctx->where()->expression(), desc);
				rootOp_ = planner_->filter(std::move(rootOp_), predicate, desc);
			} catch (const std::exception &e) {
				std::cerr << "Warning: Failed to parse WHERE clause optimization: " << e.what() << std::endl;
				// Fallback or rethrow depending on strictness
				throw;
			}
		}

		return std::any();
	}

	// --- CREATE ---
	std::any CypherToPlanVisitor::visitCreateStatement(CypherParser::CreateStatementContext *ctx) {
		auto pattern = ctx->pattern();
		if (!pattern)
			return std::any();

		for (auto part: pattern->patternPart()) {
			auto element = part->patternElement();
			auto headNodePat = element->nodePattern();

			std::string headVar = extractVariable(headNodePat->variable());
			std::string headLabel = extractLabel(headNodePat->nodeLabels());
			auto headProps = extractProperties(headNodePat->properties());

			auto headOp = planner_->create(headVar, headLabel, headProps);
			chainOperator(std::move(headOp));

			std::string prevVar = headVar;

			for (auto chain: element->patternElementChain()) {
				auto relPat = chain->relationshipPattern();
				auto targetNodePat = chain->nodePattern();

				std::string targetVar = extractVariable(targetNodePat->variable());
				std::string targetLabel = extractLabel(targetNodePat->nodeLabels());
				auto targetProps = extractProperties(targetNodePat->properties());

				auto targetOp = planner_->create(targetVar, targetLabel, targetProps);
				chainOperator(std::move(targetOp));

				auto relDetail = relPat->relationshipDetail();
				std::string edgeVar = "", edgeLabel = "";
				std::unordered_map<std::string, graph::PropertyValue> edgeProps;

				if (relDetail) {
					edgeVar = extractVariable(relDetail->variable());
					edgeLabel = extractRelType(relDetail->relationshipTypes());
					edgeProps = extractProperties(relDetail->properties());
				}

				auto edgeOp = planner_->create(edgeVar, edgeLabel, edgeProps, prevVar, targetVar);
				chainOperator(std::move(edgeOp));

				prevVar = targetVar;
			}
		}
		return std::any();
	}

	// --- RETURN ---
	std::any CypherToPlanVisitor::visitReturnStatement(CypherParser::ReturnStatementContext *ctx) {
		auto body = ctx->projectionBody();
		auto items = body->projectionItems();

		if (items->MULTIPLY()) {
			// RETURN *
		} else {
			std::vector<std::string> vars;
			for (auto item: items->projectionItem()) {
				std::string exprText = item->expression()->getText();
				vars.push_back(exprText);
			}
			if (!vars.empty()) {
				rootOp_ = planner_->project(std::move(rootOp_), vars);
			}
		}
		return std::any();
	}

	// --- CALL ---
	std::any CypherToPlanVisitor::visitStandaloneCallStatement(CypherParser::StandaloneCallStatementContext *ctx) {
		if (ctx->explicitProcedureInvocation()) {
			auto invoc = ctx->explicitProcedureInvocation();
			std::string procName = invoc->procedureName()->getText();
			std::vector<graph::PropertyValue> args;

			if (invoc->expression().empty() == false) {
				for (auto expr: invoc->expression()) {
					// Quick parse logic
					std::string txt = expr->getText();
					if (txt.front() == '\'' || txt.front() == '"') {
						args.emplace_back(txt.substr(1, txt.length() - 2));
					} else if (txt == "TRUE" || txt == "true") {
						args.emplace_back(true);
					} else if (txt == "FALSE" || txt == "false") {
						args.emplace_back(false);
					} else {
						try {
							if (txt.find('.') != std::string::npos)
								args.emplace_back(std::stod(txt));
							else
								args.emplace_back(std::stoll(txt));
						} catch (...) {
							args.emplace_back(txt);
						}
					}
				}
			}
			auto op = planner_->callProcedure(procName, args);
			chainOperator(std::move(op));
		}
		return std::any();
	}

	// --- ADMIN ---
	std::any CypherToPlanVisitor::visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext *ctx) {
		chainOperator(planner_->showIndexes());
		return std::any();
	}

	std::any CypherToPlanVisitor::visitDropIndexStatement(CypherParser::DropIndexStatementContext *ctx) {
		std::string label = extractLabelFromNodeLabel(ctx->nodeLabel());

		std::string prop = ctx->propertyKeyName()->getText();

		chainOperator(planner_->dropIndex(label, prop));
		return std::any();
	}

	std::any CypherToPlanVisitor::visitCreateIndexStatement(CypherParser::CreateIndexStatementContext *ctx) {

		std::string label = extractLabelFromNodeLabel(ctx->nodeLabel());

		std::string prop = ctx->propertyKeyName()->getText();

		chainOperator(planner_->createIndex(label, prop));
		return std::any();
	}

	// --- HELPERS ---

	std::string CypherToPlanVisitor::extractVariable(CypherParser::VariableContext *ctx) {
		return ctx ? ctx->getText() : "";
	}

	std::string CypherToPlanVisitor::extractLabel(CypherParser::NodeLabelsContext *ctx) {
		if (!ctx || ctx->nodeLabel().empty())
			return "";
		return ctx->nodeLabel(0)->labelName()->getText();
	}

	std::string CypherToPlanVisitor::extractLabelFromNodeLabel(CypherParser::NodeLabelContext *ctx) {
		return ctx ? ctx->labelName()->getText() : "";
	}

	std::string CypherToPlanVisitor::extractRelType(CypherParser::RelationshipTypesContext *ctx) {
		if (!ctx || ctx->relTypeName().empty())
			return "";
		return ctx->relTypeName(0)->getText();
	}

	PropertyValue CypherToPlanVisitor::parseValue(CypherParser::LiteralContext *ctx) {
		if (!ctx)
			return PropertyValue();
		if (ctx->StringLiteral()) {
			std::string s = ctx->StringLiteral()->getText();
			return PropertyValue(s.substr(1, s.length() - 2));
		}
		if (ctx->numberLiteral()) {
			std::string s = ctx->numberLiteral()->getText();
			if (s.find('.') != std::string::npos || s.find('e') != std::string::npos)
				return PropertyValue(std::stod(s));
			return PropertyValue(std::stoll(s));
		}
		if (ctx->booleanLiteral()) {
			return PropertyValue(ctx->booleanLiteral()->K_TRUE() != nullptr);
		}
		if (ctx->K_NULL())
			return PropertyValue();
		return PropertyValue();
	}

	std::unordered_map<std::string, PropertyValue>
	CypherToPlanVisitor::extractProperties(CypherParser::PropertiesContext *ctx) {
		std::unordered_map<std::string, PropertyValue> props;
		if (!ctx || !ctx->mapLiteral())
			return props;

		auto mapLit = ctx->mapLiteral();
		auto keys = mapLit->propertyKeyName();
		auto exprs = mapLit->expression();

		for (size_t i = 0; i < keys.size(); ++i) {
			std::string key = keys[i]->getText();
			std::string valStr = exprs[i]->getText();

			// Simplified parsing
			if (valStr.front() == '\'' || valStr.front() == '"') {
				props.emplace(key, valStr.substr(1, valStr.length() - 2));
			} else if (valStr == "TRUE" || valStr == "true") {
				props.emplace(key, true);
			} else if (valStr == "FALSE" || valStr == "false") {
				props.emplace(key, false);
			} else {
				try {
					if (valStr.find('.') != std::string::npos)
						props.emplace(key, std::stod(valStr));
					else
						props.emplace(key, std::stoll(valStr));
				} catch (...) {
					props.emplace(key, valStr);
				}
			}
		}
		return props;
	}

} // namespace graph::parser::cypher
