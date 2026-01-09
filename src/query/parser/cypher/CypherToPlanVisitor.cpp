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
#include "graph/query/execution/operators/NodeScanOperator.hpp"
#include "graph/query/execution/operators/ProjectOperator.hpp"

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

        // Case 1: Write Operators (Pipe) -> Wrap the current root
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
        if (!pattern) return std::any();

        auto parts = pattern->patternPart();
        if (parts.empty()) return std::any();

        // Iterate over all pattern parts (e.g. MATCH (a), (b))
        for (auto part : parts) {
            auto element = part->patternElement();

            // ---------------------------------------------------------
            // 1. Head Node Processing (Scan)
            // ---------------------------------------------------------
            auto headNodePat = element->nodePattern();

            std::string var = extractVariable(headNodePat->variable());
            std::string label = extractLabel(headNodePat->nodeLabels());
            auto props = extractProperties(headNodePat->properties());

            // --- Optimizer: Index Pushdown ---
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

            // Create Sub-Plan for this component
            auto currentOp = planner_->scanOp(var, label, pushKey, pushVal);

            // Apply Filters
            for (const auto &[key, val]: residualFilters) {
                auto predicate = [var, key, val](const query::execution::Record &r) {
                    auto n = r.getNode(var);
                    if (!n) return false;
                    const auto &p = n->getProperties();
                    return p.contains(key) && p.at(key) == val;
                };
                std::string desc = var + "." + key + " == " + val.toString();
                currentOp = planner_->filterOp(std::move(currentOp), predicate, desc);
            }

            // ---------------------------------------------------------
            // Merge with Global Pipeline
            // ---------------------------------------------------------
            if (!rootOp_) {
                // First component: It becomes the root
                rootOp_ = std::move(currentOp);
            } else {
                // Subsequent component: Cross Join with existing root
                // This ensures MATCH (a) MATCH (b) keeps both 'a' and 'b'
                rootOp_ = planner_->cartesianProductOp(std::move(rootOp_), std::move(currentOp));
            }

            // ---------------------------------------------------------
            // 2. Traversal Chain (e.g. -[r]->(b))
            // ---------------------------------------------------------
            // Note: We extend 'rootOp_' directly now, as it contains the source 'var'
            for (auto chain: element->patternElementChain()) {
                auto relPat = chain->relationshipPattern();
                auto nodePat = chain->nodePattern();

                // Direction
                std::string direction = "both";
                bool hasLeft = (relPat->LT() != nullptr);
                bool hasRight = (relPat->GT() != nullptr);
                if (hasLeft && !hasRight) direction = "in";
                else if (!hasLeft && hasRight) direction = "out";

                // Rel Details
                auto relDetail = relPat->relationshipDetail();
                std::string edgeVar = "", edgeLabel = "";
                std::unordered_map<std::string, graph::PropertyValue> edgeProps;

                bool isVarLength = false;
                int minHops = 1;
                int maxHops = 1;

                if (relDetail) {
                    edgeVar = extractVariable(relDetail->variable());
                    edgeLabel = extractRelType(relDetail->relationshipTypes());
                    edgeProps = extractProperties(relDetail->properties());

                    // Variable Length Logic
                    if (relDetail->rangeLiteral()) {
                        isVarLength = true;
                        auto range = relDetail->rangeLiteral();
                        minHops = 1; maxHops = 15; // Defaults

                        auto ints = range->integerLiteral();
                        bool hasRangeDots = (range->RANGE() != nullptr);

                        if (hasRangeDots) {
                            if (ints.size() == 2) { // *1..5
                                minHops = std::stoi(ints[0]->getText());
                                maxHops = std::stoi(ints[1]->getText());
                            } else if (ints.size() == 1) {
                                std::string rawText = range->getText();
                                if (rawText.find("..") < rawText.find(ints[0]->getText())) {
                                    // ..5
                                    maxHops = std::stoi(ints[0]->getText());
                                } else {
                                    // 1..
                                    minHops = std::stoi(ints[0]->getText());
                                }
                            }
                        } else if (!ints.empty()) {
                            // *3
                            minHops = maxHops = std::stoi(ints[0]->getText());
                        }
                    }
                }

                // Target Node
                std::string targetVar = extractVariable(nodePat->variable());
                std::string targetLabel = extractLabel(nodePat->nodeLabels());
                auto targetProps = extractProperties(nodePat->properties());

                // Build Traversal
                if (isVarLength) {
                    rootOp_ = planner_->traverseVarLengthOp(
                        std::move(rootOp_), var, targetVar, edgeLabel, minHops, maxHops, direction
                    );
                } else {
                    rootOp_ = planner_->traverseOp(
                        std::move(rootOp_), var, edgeVar, targetVar, edgeLabel, direction
                    );
                }

                // Filter Target
                if (!targetLabel.empty()) {
                    auto labelPred = [targetVar, targetLabel](const query::execution::Record &r) {
                        auto n = r.getNode(targetVar);
                        return n && n->getLabel() == targetLabel;
                    };
                    rootOp_ = planner_->filterOp(std::move(rootOp_), labelPred, "Label(" + targetVar + ")=" + targetLabel);
                }

                for (const auto &[key, val]: targetProps) {
                    auto pred = [targetVar, key, val](const query::execution::Record &r) {
                        auto n = r.getNode(targetVar);
                        return n && n->getProperties().contains(key) && n->getProperties().at(key) == val;
                    };
                    rootOp_ = planner_->filterOp(std::move(rootOp_), pred, targetVar + "." + key + "=" + val.toString());
                }

                // Filter Edge (Single Hop Only)
                if (!isVarLength) {
                    for (const auto &[key, val]: edgeProps) {
                        auto pred = [edgeVar, key, val](const query::execution::Record &r) {
                            auto e = r.getEdge(edgeVar);
                            return e && e->getProperties().contains(key) && e->getProperties().at(key) == val;
                        };
                        rootOp_ = planner_->filterOp(std::move(rootOp_), pred, edgeVar + "." + key + "=" + val.toString());
                    }
                }

                // Update current variable for next hop
                var = targetVar;
            }
        } // End Parts Loop

        // 3. Where Clause
        if (ctx->where()) {
            try {
                std::string desc;
                auto predicate = buildWherePredicate(ctx->where()->expression(), desc);
                rootOp_ = planner_->filterOp(std::move(rootOp_), predicate, desc);
            } catch (const std::exception &e) {
                std::cerr << "Warning: Failed to parse WHERE clause optimization: " << e.what() << std::endl;
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

			auto headOp = planner_->createOp(headVar, headLabel, headProps);
			chainOperator(std::move(headOp));

			std::string prevVar = headVar;

			for (auto chain: element->patternElementChain()) {
				auto relPat = chain->relationshipPattern();
				auto targetNodePat = chain->nodePattern();

				std::string targetVar = extractVariable(targetNodePat->variable());
				std::string targetLabel = extractLabel(targetNodePat->nodeLabels());
				auto targetProps = extractProperties(targetNodePat->properties());

				auto targetOp = planner_->createOp(targetVar, targetLabel, targetProps);
				chainOperator(std::move(targetOp));

				auto relDetail = relPat->relationshipDetail();
				std::string edgeVar = "", edgeLabel = "";
				std::unordered_map<std::string, graph::PropertyValue> edgeProps;

				if (relDetail) {
					edgeVar = extractVariable(relDetail->variable());
					edgeLabel = extractRelType(relDetail->relationshipTypes());
					edgeProps = extractProperties(relDetail->properties());
				}

				auto edgeOp = planner_->createOp(edgeVar, edgeLabel, edgeProps, prevVar, targetVar);
				chainOperator(std::move(edgeOp));

				prevVar = targetVar;
			}
		}
		return std::any();
	}

	// --- RETURN ---
	std::any CypherToPlanVisitor::visitReturnStatement(CypherParser::ReturnStatementContext *ctx) {
		auto body = ctx->projectionBody();

		// FIX: If there is no existing plan (e.g. standalone RETURN 1),
		// inject a SingleRowOperator to provide a valid pipeline source.
		// This generates exactly one empty record to allow projection of literals.
		if (!rootOp_) {
			rootOp_ = planner_->singleRowOp();
		}

		// Handle Projection
		auto items = body->projectionItems();
		if (!items->MULTIPLY()) { // If not RETURN *
			std::vector<query::execution::operators::ProjectItem> projItems;

			for (auto item: items->projectionItem()) {
				// 1. Expression Text (Source)
				std::string exprText = item->expression()->getText();

				// 2. Alias (Output Name)
				// If 'AS alias' is present, use it. Otherwise, use expression text.
				std::string alias = exprText;
				if (item->K_AS()) {
					alias = item->variable()->getText();
				}

				projItems.push_back({exprText, alias});
			}

			if (!projItems.empty()) {
				// Note: Ensure QueryPlanner::projectOp is updated to accept vector<ProjectItem>
				rootOp_ = planner_->projectOp(std::move(rootOp_), projItems);
			}
		}

		// Order By
		if (body->orderStatement()) {
			std::vector<query::execution::operators::SortItem> sortItems;
			auto sortItemList = body->orderStatement()->sortItem();

			for (auto item: sortItemList) {
				// Parse Expression (n.age)
				std::string varName;
				std::string propName;

				// Hacky parse for "n.prop" string
				// In production, use a proper ExpressionVisitor
				std::string text = item->expression()->getText();
				size_t dotPos = text.find('.');
				if (dotPos != std::string::npos) {
					varName = text.substr(0, dotPos);
					propName = text.substr(dotPos + 1);
				} else {
					varName = text; // Just "n" (Sort by ID)
				}

				// Determine Direction
				bool asc = true;
				if (item->K_DESC() || item->K_DESCENDING()) {
					asc = false;
				}

				sortItems.push_back({varName, propName, asc});
			}

			if (!sortItems.empty()) {
				rootOp_ = planner_->sortOp(std::move(rootOp_), sortItems);
			}
		}

		// Handle SKIP
		if (body->skipStatement()) {
			auto skipExpr = body->skipStatement()->expression();
			int64_t offset = 0;
			try {
				offset = std::stoll(skipExpr->getText());
			} catch (...) {
				throw std::runtime_error("SKIP requires an integer literal.");
			}

			rootOp_ = planner_->skipOp(std::move(rootOp_), offset);
		}

		// Handle LIMIT
		if (body->limitStatement()) {
			auto limitExpr = body->limitStatement()->expression();
			int64_t limit = 0;
			try {
				limit = std::stoll(limitExpr->getText());
			} catch (...) {
				throw std::runtime_error("LIMIT requires an integer literal.");
			}

			rootOp_ = planner_->limitOp(std::move(rootOp_), limit);
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
			auto op = planner_->callProcedureOp(procName, args);
			chainOperator(std::move(op));
		}
		return std::any();
	}

	// --- ADMIN ---
	std::any CypherToPlanVisitor::visitShowIndexesStatement(CypherParser::ShowIndexesStatementContext * /*ctx*/) {
		chainOperator(planner_->showIndexesOp());
		return std::any();
	}

	// ========================================================================
	// CREATE INDEX
	// ========================================================================
	std::any CypherToPlanVisitor::visitCreateIndexByPattern(CypherParser::CreateIndexByPatternContext *ctx) {
		// Syntax: CREATE INDEX [name] FOR (n:Label) ON (n.prop)

		// 1. Name (Optional)
		std::string name = "";
		if (ctx->symbolicName()) {
			name = ctx->symbolicName()->getText();
		}

		// 2. Label (From Pattern)
		std::string label;
		auto pat = ctx->nodePattern();
		if (pat->nodeLabels() && !pat->nodeLabels()->nodeLabel().empty()) {
			label = extractLabelFromNodeLabel(pat->nodeLabels()->nodeLabel(0));
		} else {
			throw std::runtime_error("CREATE INDEX pattern must specify a Label (e.g. :User)");
		}

		// 3. Property
		std::string prop = extractPropertyKeyFromExpr(ctx->propertyExpression());

		chainOperator(planner_->createIndexOp(name, label, prop));
		return std::any();
	}

	std::string CypherToPlanVisitor::extractPropertyKeyFromExpr(CypherParser::PropertyExpressionContext *ctx) {
		if (!ctx)
			return "";

		// propertyExpression -> atom (DOT propertyKeyName)*

		// Case A: n.age
		if (!ctx->propertyKeyName().empty()) {
			// Return the last part (the key)
			return ctx->propertyKeyName().back()->getText();
		}

		// Case B: age (legacy or simplified syntax)
		// If no DOTs, the key is inside the atom (variable)
		return ctx->atom()->getText();
	}

	std::any CypherToPlanVisitor::visitCreateIndexByLabel(CypherParser::CreateIndexByLabelContext *ctx) {
		// Syntax: CREATE INDEX [name] ON :Label(prop)
		std::string name = "";
		if (ctx->symbolicName()) {
			name = ctx->symbolicName()->getText();
		}

		std::string label = extractLabelFromNodeLabel(ctx->nodeLabel());

		std::string prop = ctx->propertyKeyName()->getText();

		chainOperator(planner_->createIndexOp(name, label, prop));
		return std::any();
	}

	// ========================================================================
	// DROP INDEX
	// ========================================================================
	std::any CypherToPlanVisitor::visitDropIndexByName(CypherParser::DropIndexByNameContext *ctx) {
		// Syntax: DROP INDEX name
		std::string name = ctx->symbolicName()->getText();
		chainOperator(planner_->dropIndexOp(name));
		return std::any();
	}

	std::any CypherToPlanVisitor::visitDropIndexByLabel(CypherParser::DropIndexByLabelContext *ctx) {
		// Syntax: DROP INDEX ON :Label(prop)
		std::string label = extractLabelFromNodeLabel(ctx->nodeLabel());

		std::string prop = ctx->propertyKeyName()->getText();

		chainOperator(planner_->dropIndexOp(label, prop));
		return std::any();
	}

	// --- HELPERS ---

	// --- DELETE ---
	std::any CypherToPlanVisitor::visitDeleteStatement(CypherParser::DeleteStatementContext *ctx) {
		// Grammar: DETACH? DELETE expression (COMMA expression)*

		bool detach = (ctx->K_DETACH() != nullptr);
		std::vector<std::string> vars;

		for (auto expr: ctx->expression()) {
			// Simplified: Expect expression to be a variable name (Atom -> Variable)
			// Need to extract text.
			// For now, getText() is a reasonable approximation for variable names.
			vars.push_back(expr->getText());
		}

		if (!rootOp_) {
			throw std::runtime_error("DELETE cannot be the start of a query. Use MATCH first.");
		}

		rootOp_ = planner_->deleteOp(std::move(rootOp_), vars, detach);
		return std::any();
	}

	// --- SET ---
	std::any CypherToPlanVisitor::visitSetStatement(CypherParser::SetStatementContext *ctx) {
		// Prepare a list of items to update
		std::vector<query::execution::operators::SetItem> items;

		// Iterate through all comma-separated items: SET n.a=1, n:Label
		for (auto item: ctx->setItem()) {

			// --- Case 1: Property Assignment (n.prop = val) ---
			// Grammar: propertyExpression EQ expression
			if (item->propertyExpression() && item->EQ()) {
				auto propExpr = item->propertyExpression();

				// 1. Extract Variable (n)
				// propertyExpression -> atom -> variable
				std::string varName = "";
				if (propExpr->atom() && propExpr->atom()->variable()) {
					varName = extractVariable(propExpr->atom()->variable());
				} else {
					throw std::runtime_error("SET property must start with a variable (e.g. n.prop)");
				}

				// 2. Extract Key (prop)
				// Using the helper defined previously
				std::string keyName = extractPropertyKeyFromExpr(propExpr);

				// 3. Extract Value (val)
				// Note: In a full engine, we use an ExpressionVisitor.
				// Here we use simplified literal parsing based on getText().
				std::string valText = item->expression()->getText();
				graph::PropertyValue val;

				if (valText.front() == '\'' || valText.front() == '"') {
					// String literal
					val = graph::PropertyValue(valText.substr(1, valText.length() - 2));
				} else if (valText == "TRUE" || valText == "true") {
					val = graph::PropertyValue(true);
				} else if (valText == "FALSE" || valText == "false") {
					val = graph::PropertyValue(false);
				} else {
					// Try numeric parsing
					try {
						if (valText.find('.') != std::string::npos) {
							val = graph::PropertyValue(std::stod(valText));
						} else {
							val = graph::PropertyValue(std::stoll(valText));
						}
					} catch (...) {
						// Fallback: If it's a parameter ($param) or complex expr,
						// we currently store it as a string for debugging.
						// Ideally throw error if not supported.
						val = graph::PropertyValue(valText);
					}
				}

				// Add to list with PROPERTY type
				items.push_back({query::execution::operators::SetActionType::PROPERTY, varName, keyName, val});
			}

			// --- Case 2: Label Assignment (n:Label) ---
			// Grammar: variable nodeLabels
			else if (item->variable() && item->nodeLabels()) {
				std::string varName = extractVariable(item->variable());
				std::string labelName = extractLabel(item->nodeLabels());

				// Add to list with LABEL type (value is ignored/empty)
				items.push_back(
						{query::execution::operators::SetActionType::LABEL, varName, labelName, PropertyValue()});
			}

			// --- Case 3: Other forms (+=, etc.) are not yet supported ---
			else {
				// You might want to log a warning or throw
				// throw std::runtime_error("Unsupported SET syntax (e.g. += or map assignment)");
			}
		}

		// Ensure we have a valid pipeline to operate on
		if (!rootOp_) {
			throw std::runtime_error("SET clause must follow a MATCH or CREATE clause.");
		}

		// Create and chain the SetOperator
		rootOp_ = planner_->setOp(std::move(rootOp_), items);

		return std::any();
	}

	std::any CypherToPlanVisitor::visitRemoveStatement(CypherParser::RemoveStatementContext *ctx) {
		std::vector<query::execution::operators::RemoveItem> items;

		for (auto item: ctx->removeItem()) {
			// Case 1: n.prop (Property)
			if (item->propertyExpression()) {
				auto propExpr = item->propertyExpression();
				std::string varName = propExpr->atom()->getText();
				std::string keyName = propExpr->propertyKeyName(0)->getText();
				items.push_back({query::execution::operators::RemoveActionType::PROPERTY, varName, keyName});
			}
			// Case 2: n:Label (Label)
			else if (item->variable() && item->nodeLabels()) {
				std::string varName = extractVariable(item->variable());
				std::string labelName = extractLabel(item->nodeLabels());
				items.push_back({query::execution::operators::RemoveActionType::LABEL, varName, labelName});
			}
		}

		if (!rootOp_)
			throw std::runtime_error("REMOVE must follow a MATCH or CREATE");
		rootOp_ = planner_->removeOp(std::move(rootOp_), items);
		return std::any();
	}

	// --- MERGE ---
	std::any CypherToPlanVisitor::visitMergeStatement(CypherParser::MergeStatementContext *ctx) {
		// Grammar: MERGE patternPart ( ON (MATCH|CREATE) setClause )*

		auto patternPart = ctx->patternPart();
		// MERGE only supports a single pattern part usually
		auto element = patternPart->patternElement();

		// 1. Head Node Extraction (Similar to MATCH/CREATE)
		auto headNodePat = element->nodePattern();
		std::string var = extractVariable(headNodePat->variable());
		std::string label = extractLabel(headNodePat->nodeLabels());
		auto matchProps = extractProperties(headNodePat->properties());

		// 2. Parse Actions
		std::vector<query::execution::operators::SetItem> onCreateItems;
		std::vector<query::execution::operators::SetItem> onMatchItems;

		// Iterate children to find ON MATCH / ON CREATE blocks
		// The grammar structure: ( K_ON ( K_MATCH | K_CREATE ) setClause )*
		// Since ANTLR flattens lists, we need to iterate carefully or use context methods if named

		// Easier approach: Iterate children
		for (size_t i = 0; i < ctx->children.size(); ++i) {
			if (ctx->children[i]->getText() == "ON") {
				// Next is MATCH or CREATE
				std::string type = ctx->children[i + 1]->getText(); // MATCH / CREATE
				// Next is SET clause (Parser rule context)
				auto setCtx = dynamic_cast<CypherParser::SetStatementContext *>(ctx->children[i + 2]);

				auto items = extractSetItems(setCtx);
				if (type == "MATCH") {
					onMatchItems.insert(onMatchItems.end(), items.begin(), items.end());
				} else if (type == "CREATE") {
					onCreateItems.insert(onCreateItems.end(), items.begin(), items.end());
				}
			}
		}

		// 3. Build Operator
		// Currently only supporting MERGE (n) (Single Node)
		// Edge Merge requires traversal logic which is more complex.

		auto op = planner_->mergeOp(var, label, matchProps, onCreateItems, onMatchItems);
		chainOperator(std::move(op));

		// TODO: Handle pattern chains -[r]->(m)

		return std::any();
	}

	// --- Helper: Extract Set Items ---
	std::vector<query::execution::operators::SetItem>
	CypherToPlanVisitor::extractSetItems(CypherParser::SetStatementContext *ctx) {
		std::vector<query::execution::operators::SetItem> items;
		if (!ctx)
			return items;

		for (auto item: ctx->setItem()) {

			// --- Case 1: Property Update (n.prop = val) ---
			if (item->propertyExpression() && item->EQ()) {
				auto propExpr = item->propertyExpression();

				// 1. Extract Variable
				std::string varName = "";
				if (propExpr->atom() && propExpr->atom()->variable()) {
					varName = extractVariable(propExpr->atom()->variable());
				}

				// 2. Extract Key
				std::string keyName = extractPropertyKeyFromExpr(propExpr);

				// 3. Extract Value
				// Reusing the text parsing logic for simplicity
				std::string valText = item->expression()->getText();
				graph::PropertyValue val;

				if (valText.front() == '\'' || valText.front() == '"') {
					val = graph::PropertyValue(valText.substr(1, valText.length() - 2));
				} else if (valText == "TRUE" || valText == "true") {
					val = graph::PropertyValue(true);
				} else if (valText == "FALSE" || valText == "false") {
					val = graph::PropertyValue(false);
				} else {
					try {
						if (valText.find('.') != std::string::npos)
							val = graph::PropertyValue(std::stod(valText));
						else
							val = graph::PropertyValue(std::stoll(valText));
					} catch (...) {
						val = graph::PropertyValue(valText);
					}
				}

				// Added SetActionType::PROPERTY as first argument
				items.push_back({query::execution::operators::SetActionType::PROPERTY, varName, keyName, val});
			}

			// --- Case 2: Label Assignment (n:Label) ---
			else if (item->variable() && item->nodeLabels()) {
				std::string varName = extractVariable(item->variable());
				std::string labelName = extractLabel(item->nodeLabels());

				// Handle Label setting in MERGE actions
				items.push_back({
						query::execution::operators::SetActionType::LABEL, varName, labelName,
						graph::PropertyValue() // Empty value for labels
				});
			}
		}
		return items;
	}

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

	// ========================================================================
    // UNWIND Clause
    // ========================================================================
    std::any CypherToPlanVisitor::visitUnwindStatement(CypherParser::UnwindStatementContext *ctx) {
        // Grammar: UNWIND expression AS variable

        // 1. Extract Alias
        std::string alias = extractVariable(ctx->variable());
        if (alias.empty()) {
            throw std::runtime_error("UNWIND requires a variable alias (AS x)");
        }

        // 2. Extract List Values
        // We need to parse the expression. Currently, we support List Literals: [1, 2, 3]
        std::vector<PropertyValue> listValues;

        auto expr = ctx->expression();
        if (expr) {
            listValues = extractListFromExpression(expr);
        }

        if (listValues.empty()) {
            // If parsing failed or list is empty, we might want to throw or allow empty unwind (which stops execution)
            // For now, let's proceed. An empty list in UNWIND stops the pipeline (0 rows).
        }

		if (!rootOp_) {
			rootOp_ = planner_->singleRowOp();
		}

        // 3. Build Operator
        // UNWIND wraps the current pipeline (rootOp_)
        // If rootOp_ is null, UNWIND acts as a Source.
        auto op = planner_->unwindOp(std::move(rootOp_), alias, listValues);

        // Update root
        rootOp_ = std::move(op);

        return std::any();
    }

    // ========================================================================
    // Helper: Extract List from Expression
    // ========================================================================
    std::vector<PropertyValue>
    CypherToPlanVisitor::extractListFromExpression(CypherParser::ExpressionContext *ctx) {
        std::vector<graph::PropertyValue> results;

        // Navigate AST: expression -> or -> xor -> and -> not -> comparison -> arithmetic -> unary -> atom
        // This is a long chain. We check if the expression effectively boils down to a listLiteral.

        // Safety check helper
        auto getAtom = [](CypherParser::ExpressionContext* e) -> CypherParser::AtomContext* {
            if (!e) return nullptr;
            auto or_ = e->orExpression(); if(!or_) return nullptr;
            auto xor_ = or_->xorExpression(0); if(!xor_) return nullptr;
            auto and_ = xor_->andExpression(0); if(!and_) return nullptr;
            auto not_ = and_->notExpression(0); if(!not_) return nullptr;
            auto comp = not_->comparisonExpression(); if(!comp) return nullptr;
            auto arith = comp->arithmeticExpression(0); if(!arith) return nullptr;
            auto unary = arith->unaryExpression(0); if(!unary) return nullptr;
            return unary->atom();
        };

        auto atom = getAtom(ctx);
        if (atom && atom->listLiteral()) {
            auto listLit = atom->listLiteral();
            // listLiteral : LBRACK ( expression ( COMMA expression )* )? RBRACK

            for (auto itemExpr : listLit->expression()) {
                // Recursively extract literal value from the item expression
                auto itemAtom = getAtom(itemExpr);
                if (itemAtom && itemAtom->literal()) {
                    results.push_back(parseValue(itemAtom->literal()));
                } else {
                    // Fallback for non-literal items inside list (e.g. identifiers)
                    // Currently simplified to string representation
                    results.push_back(graph::PropertyValue(itemExpr->getText()));
                }
            }
        } else {
            // Attempt to parse parameter ($param) if implemented, or throw
            // For now, if it's not a list literal, we throw or return empty
            // throw std::runtime_error("UNWIND currently only supports List Literals (e.g. [1, 2])");
        }

        return results;
    }

} // namespace graph::parser::cypher
