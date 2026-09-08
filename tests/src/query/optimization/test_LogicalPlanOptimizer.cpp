/**
 * @file test_LogicalPlanOptimizer.cpp
 * @brief Tests for storage-neutral logical-plan normalization.
 *
 * Licensed under the Apache License, Version 2.0
 **/

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/query/expressions/Expression.hpp"
#include "graph/query/logical/operators/LogicalFilter.hpp"
#include "graph/query/logical/operators/LogicalJoin.hpp"
#include "graph/query/logical/operators/LogicalLimit.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"
#include "graph/query/logical/operators/LogicalSkip.hpp"
#include "graph/query/optimization/LogicalPlanOptimizer.hpp"

namespace graph::query::optimization {
	namespace {

		class FixedStatisticsProvider final : public LogicalStatisticsProvider {
		public:
			explicit FixedStatisticsProvider(const LogicalCardinalityEstimate estimate) : estimate_(estimate) {}

			[[nodiscard]] std::optional<LogicalCardinalityEstimate>
			estimate(const logical::LogicalOperator &) const noexcept override {
				return estimate_;
			}

		private:
			LogicalCardinalityEstimate estimate_;
		};

		class NoEstimateStatisticsProvider final : public LogicalStatisticsProvider {
		public:
			[[nodiscard]] std::optional<LogicalCardinalityEstimate>
			estimate(const logical::LogicalOperator &) const noexcept override {
				return std::nullopt;
			}
		};

		class LifetimeStatisticsProvider final : public LogicalStatisticsProvider {
		public:
			explicit LifetimeStatisticsProvider(bool &destroyed) : destroyed_(destroyed) {}

			~LifetimeStatisticsProvider() override { destroyed_ = true; }

			[[nodiscard]] std::optional<LogicalCardinalityEstimate>
			estimate(const logical::LogicalOperator &) const noexcept override {
				return std::nullopt;
			}

		private:
			bool &destroyed_;
		};

		class VariableStatisticsProvider final : public LogicalStatisticsProvider {
		public:
			[[nodiscard]] std::optional<LogicalCardinalityEstimate>
			estimate(const logical::LogicalOperator &root) const noexcept override {
				const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(&root);
				if (scan == nullptr) {
					return LogicalCardinalityEstimate{0U, 100U, 100U};
				}
				if (scan->getVariable() == "small") {
					return LogicalCardinalityEstimate{0U, 2U, 2U};
				}
				if (scan->getVariable() == "large") {
					return LogicalCardinalityEstimate{0U, 50U, 50U};
				}
				return std::nullopt;
			}
		};

		TEST(LogicalCardinalityEstimateTest, AcceptsOrderedBoundsAndRejectsInconsistentEstimates) {
			EXPECT_TRUE((LogicalCardinalityEstimate{0U, 0U, 0U}.isValid()));
			EXPECT_TRUE((LogicalCardinalityEstimate{3U, 5U, 8U}.isValid()));
			EXPECT_FALSE((LogicalCardinalityEstimate{6U, 5U, 8U}.isValid()));
			EXPECT_FALSE((LogicalCardinalityEstimate{3U, 9U, 8U}.isValid()));
		}

		TEST(LogicalStatisticsProviderContractTest, DestroysProvidersPolymorphically) {
			bool destroyed = false;
			{
				std::unique_ptr<LogicalStatisticsProvider> provider =
						std::make_unique<LifetimeStatisticsProvider>(destroyed);
				EXPECT_FALSE(destroyed);
			}
			EXPECT_TRUE(destroyed);
		}

		class DeclaredLimitWithoutLimitImplementation final : public logical::LogicalOperator {
		public:
			[[nodiscard]] logical::LogicalOpType getType() const override { return logical::LogicalOpType::LOP_LIMIT; }
			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<DeclaredLimitWithoutLimitImplementation>();
			}
			[[nodiscard]] std::string toString() const override { return "BrokenLimit"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
		};

		class DeclaredSkipWithoutSkipImplementation final : public logical::LogicalOperator {
		public:
			[[nodiscard]] logical::LogicalOpType getType() const override { return logical::LogicalOpType::LOP_SKIP; }
			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<DeclaredSkipWithoutSkipImplementation>();
			}
			[[nodiscard]] std::string toString() const override { return "BrokenSkip"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
		};

		class DeclaredFilterWithoutFilterImplementation final : public logical::LogicalOperator {
		public:
			[[nodiscard]] logical::LogicalOpType getType() const override { return logical::LogicalOpType::LOP_FILTER; }
			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<DeclaredFilterWithoutFilterImplementation>();
			}
			[[nodiscard]] std::string toString() const override { return "BrokenFilter"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
		};

		class DeclaredNodeScanWithoutNodeScanImplementation final : public logical::LogicalOperator {
		public:
			[[nodiscard]] logical::LogicalOpType getType() const override {
				return logical::LogicalOpType::LOP_NODE_SCAN;
			}
			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
			[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"n"}; }
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> clone() const override {
				return std::make_unique<DeclaredNodeScanWithoutNodeScanImplementation>();
			}
			[[nodiscard]] std::string toString() const override { return "BrokenNodeScan"; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
		};

		class LogicalFilterWithNoChildren final : public logical::LogicalFilter {
		public:
			LogicalFilterWithNoChildren() : LogicalFilter(nullptr, std::make_shared<expressions::LiteralExpression>(true)) {}

			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override { return {}; }
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override { return nullptr; }
		};

		class LogicalFilterWithTransientNullChild final : public logical::LogicalFilter {
		public:
			LogicalFilterWithTransientNullChild(std::unique_ptr<logical::LogicalOperator> child,
												std::shared_ptr<expressions::Expression> predicate) :
					LogicalFilter(std::move(child), std::move(predicate)) {}

			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override {
				if (children_calls_++ == 0U) {
					return {const_cast<logical::LogicalNodeScan *>(&probe_)};
				}
				return {nullptr};
			}
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override {
				return std::make_unique<logical::LogicalNodeScan>("n");
			}

		private:
			mutable size_t children_calls_ = 0U;
			logical::LogicalNodeScan probe_{"n"};
		};

		class BrokenLogicalLimit final : public logical::LogicalLimit {
		public:
			explicit BrokenLogicalLimit(const size_t successfulDetaches) :
				LogicalLimit(nullptr, 1), successful_detaches_(successfulDetaches) {}

			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override {
				return {const_cast<logical::LogicalNodeScan *>(&probe_)};
			}
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override {
				if (detach_count_++ < successful_detaches_) {
					return std::make_unique<logical::LogicalNodeScan>("probe");
				}
				return nullptr;
			}

		private:
			logical::LogicalNodeScan probe_{"probe"};
			size_t successful_detaches_;
			size_t detach_count_ = 0U;
		};

		class BrokenLogicalSkip final : public logical::LogicalSkip {
		public:
			explicit BrokenLogicalSkip(const size_t successfulDetaches) :
				LogicalSkip(nullptr, 1), successful_detaches_(successfulDetaches) {}

			[[nodiscard]] std::vector<logical::LogicalOperator *> getChildren() const override {
				return {const_cast<logical::LogicalNodeScan *>(&probe_)};
			}
			void setChild(size_t, std::unique_ptr<logical::LogicalOperator>) override {}
			[[nodiscard]] std::unique_ptr<logical::LogicalOperator> detachChild(size_t) override {
				if (detach_count_++ < successful_detaches_) {
					return std::make_unique<logical::LogicalNodeScan>("probe");
				}
				return nullptr;
			}

		private:
			logical::LogicalNodeScan probe_{"probe"};
			size_t successful_detaches_;
			size_t detach_count_ = 0U;
		};

		TEST(LogicalPlanOptimizerTest, FusesNestedLimitsWithoutChangingTheScan) {
			auto root = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalLimit>(
							std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Person"}), 9),
					4);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *limit = dynamic_cast<const logical::LogicalLimit *>(result.root.get());
			ASSERT_NE(limit, nullptr);
			EXPECT_EQ(limit->getLimit(), 4);
			const auto children = limit->getChildren();
			ASSERT_EQ(children.size(), 1U);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(children.front());
			ASSERT_NE(scan, nullptr);
			EXPECT_EQ(scan->getVariable(), "node");
			ASSERT_EQ(scan->getLabels().size(), 1U);
			EXPECT_EQ(scan->getLabels().front(), "Person");
		}

		TEST(LogicalPlanOptimizerTest, LeavesAFilterWithAnInvalidChildShapeUntouched) {
			const LogicalOptimizationResult result =
					LogicalPlanOptimizer{}.optimize(std::make_unique<LogicalFilterWithNoChildren>());
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 0U);
			EXPECT_EQ(result.root->getType(), logical::LogicalOpType::LOP_FILTER);
		}

		TEST(LogicalPlanOptimizerTest, LeavesAFilterWhoseNodeScanTypeIsNotItsConcreteClassUntouched) {
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "key"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{7}));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<DeclaredNodeScanWithoutNodeScanImplementation>(), std::move(predicate));
			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, PushesLimitBelowAValidNonDistinctProjection) {
			auto projection = std::make_unique<logical::LogicalProject>(
					std::make_unique<logical::LogicalNodeScan>("node"),
					std::vector<logical::LogicalProjectItem>{
							{std::make_shared<expressions::VariableReferenceExpression>("node"), "node"}});
			auto root = std::make_unique<logical::LogicalLimit>(std::move(projection), 3);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *optimizedProjection = dynamic_cast<const logical::LogicalProject *>(result.root.get());
			ASSERT_NE(optimizedProjection, nullptr);
			EXPECT_FALSE(optimizedProjection->isDistinct());
			ASSERT_EQ(optimizedProjection->getItems().size(), 1U);
			EXPECT_EQ(optimizedProjection->getItems().front().alias, "node");
			const auto children = optimizedProjection->getChildren();
			ASSERT_EQ(children.size(), 1U);
			const auto *limit = dynamic_cast<const logical::LogicalLimit *>(children.front());
			ASSERT_NE(limit, nullptr);
			EXPECT_EQ(limit->getLimit(), 3);
			ASSERT_EQ(limit->getChildren().size(), 1U);
			EXPECT_EQ(limit->getChildren().front()->getType(), logical::LogicalOpType::LOP_NODE_SCAN);
		}

		TEST(LogicalPlanOptimizerTest, PushesPositiveLimitBelowNonNegativeSkip) {
			auto root = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalSkip>(
							std::make_unique<logical::LogicalNodeScan>("node", std::vector<std::string>{"Person"}), 5),
					3);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *skip = dynamic_cast<const logical::LogicalSkip *>(result.root.get());
			ASSERT_NE(skip, nullptr);
			EXPECT_EQ(skip->getOffset(), 5);
			ASSERT_EQ(skip->getChildren().size(), 1U);
			const auto *limit = dynamic_cast<const logical::LogicalLimit *>(skip->getChildren().front());
			ASSERT_NE(limit, nullptr);
			EXPECT_EQ(limit->getLimit(), 8);
			ASSERT_EQ(limit->getChildren().size(), 1U);
			EXPECT_EQ(limit->getChildren().front()->getType(), logical::LogicalOpType::LOP_NODE_SCAN);
		}

		TEST(LogicalPlanOptimizerTest, RetainsDistinctAndMalformedProjectionsAboveLimit) {
			const auto node = std::make_shared<expressions::VariableReferenceExpression>("node");
			auto distinct = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("node"),
															  std::vector<logical::LogicalProjectItem>{{node, "node"}},
															  true),
					3);
			const LogicalOptimizationResult distinctResult = LogicalPlanOptimizer{}.optimize(std::move(distinct));
			EXPECT_EQ(distinctResult.rewrite_count, 0U);
			ASSERT_NE(distinctResult.root, nullptr);
			EXPECT_EQ(distinctResult.root->getType(), logical::LogicalOpType::LOP_LIMIT);

			auto malformed = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalProject>(
							std::make_unique<logical::LogicalNodeScan>("node"),
							std::vector<logical::LogicalProjectItem>{{nullptr, "node"}}),
					3);
			const LogicalOptimizationResult malformedResult = LogicalPlanOptimizer{}.optimize(std::move(malformed));
			EXPECT_EQ(malformedResult.rewrite_count, 0U);
			ASSERT_NE(malformedResult.root, nullptr);
			EXPECT_EQ(malformedResult.root->getType(), logical::LogicalOpType::LOP_LIMIT);

			auto emptyAlias = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("node"),
															  std::vector<logical::LogicalProjectItem>{{node, ""}}),
					3);
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(emptyAlias)).rewrite_count, 0U);

			auto negativeLimit = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalProject>(std::make_unique<logical::LogicalNodeScan>("node"),
															  std::vector<logical::LogicalProjectItem>{{node, "node"}}),
					-1);
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(negativeLimit)).rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, FusesNestedSkipsWhenTheExactSumIsRepresentable) {
			auto root = std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"), 7), 5);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *skip = dynamic_cast<const logical::LogicalSkip *>(result.root.get());
			ASSERT_NE(skip, nullptr);
			EXPECT_EQ(skip->getOffset(), 12);
			ASSERT_EQ(skip->getChildren().size(), 1U);
			EXPECT_EQ(skip->getChildren().front()->getType(), logical::LogicalOpType::LOP_NODE_SCAN);
		}

		TEST(LogicalPlanOptimizerTest, PushesDirectLiteralPropertyEqualityIntoNodeScanWithoutRemovingFilter) {
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "email"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::LiteralExpression>(std::string{"ada@example.com"}));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *filter = dynamic_cast<const logical::LogicalFilter *>(result.root.get());
			ASSERT_NE(filter, nullptr);
			EXPECT_EQ(filter->getPredicate(), predicate);
			ASSERT_EQ(filter->getChildren().size(), 1U);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(filter->getChildren().front());
			ASSERT_NE(scan, nullptr);
			EXPECT_EQ(scan->getPreferredScanHint(), logical::LogicalScanHint::LSH_PROPERTY);
			ASSERT_EQ(scan->getPropertyPredicates().size(), 1U);
			EXPECT_EQ(scan->getPropertyPredicates().front().first, "email");
			EXPECT_EQ(scan->getPropertyPredicates().front().second, PropertyValue("ada@example.com"));
		}

		TEST(LogicalPlanOptimizerTest, PushesConjunctiveLiteralEqualitiesIntoOneNodeScanWithoutRemovingTheFilter) {
			auto age = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{42}));
			auto city = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "city"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::LiteralExpression>(std::string{"Shanghai"}));
			auto residual = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "score"),
					expressions::BinaryOperatorType::BOP_GREATER,
					std::make_unique<expressions::LiteralExpression>(int64_t{10}));
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::BinaryOpExpression>(
							std::move(age), expressions::BinaryOperatorType::BOP_AND, std::move(city)),
					expressions::BinaryOperatorType::BOP_AND, std::move(residual));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 2U);
			const auto *filter = dynamic_cast<const logical::LogicalFilter *>(result.root.get());
			ASSERT_NE(filter, nullptr);
			EXPECT_EQ(filter->getPredicate(), predicate);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(filter->getChildren().front());
			ASSERT_NE(scan, nullptr);
			EXPECT_EQ(scan->getPreferredScanHint(), logical::LogicalScanHint::LSH_RANGE);
			EXPECT_EQ(scan->getPropertyPredicates(),
					  (std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(int64_t{42})},
																	  {"city", PropertyValue("Shanghai")}}));
			ASSERT_EQ(scan->getRangePredicates().size(), 1U);
			EXPECT_EQ(scan->getRangePredicates().front().key, "score");
			EXPECT_EQ(scan->getRangePredicates().front().minValue, PropertyValue(int64_t{10}));
			EXPECT_FALSE(scan->getRangePredicates().front().minInclusive);
		}

		TEST(LogicalPlanOptimizerTest, LeavesNonExactOrAlreadyRepresentedFilterPredicatesUntouched) {
			const auto makeFilter = [](std::unique_ptr<expressions::Expression> left,
									   std::unique_ptr<expressions::Expression> right,
									   expressions::BinaryOperatorType operation,
									   std::vector<std::pair<std::string, PropertyValue>> scanPredicates = {}) {
				return std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"},
																   std::move(scanPredicates)),
						std::make_shared<expressions::BinaryOpExpression>(std::move(left), operation,
																		  std::move(right)));
			};

			auto reverse = makeFilter(std::make_unique<expressions::LiteralExpression>(int64_t{42}),
									  std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
									  expressions::BinaryOperatorType::BOP_EQUAL);
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(reverse)).rewrite_count, 0U);

			auto nullLiteral = makeFilter(std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
										  std::make_unique<expressions::LiteralExpression>(),
										  expressions::BinaryOperatorType::BOP_EQUAL);
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nullLiteral)).rewrite_count, 0U);

			auto duplicate = makeFilter(std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
										std::make_unique<expressions::LiteralExpression>(int64_t{42}),
										expressions::BinaryOperatorType::BOP_EQUAL, {{"age", PropertyValue(41)}});
			const LogicalOptimizationResult duplicateResult = LogicalPlanOptimizer{}.optimize(std::move(duplicate));
			EXPECT_EQ(duplicateResult.rewrite_count, 0U);
			const auto *duplicateFilter = dynamic_cast<const logical::LogicalFilter *>(duplicateResult.root.get());
			ASSERT_NE(duplicateFilter, nullptr);
			const auto *duplicateScan =
					dynamic_cast<const logical::LogicalNodeScan *>(duplicateFilter->getChildren().front());
			ASSERT_NE(duplicateScan, nullptr);
			ASSERT_EQ(duplicateScan->getPropertyPredicates().size(), 1U);
			EXPECT_EQ(duplicateScan->getPropertyPredicates().front().second, PropertyValue(41));

			auto parameterConflict = makeFilter(std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
												std::make_unique<expressions::LiteralExpression>(int64_t{42}),
												expressions::BinaryOperatorType::BOP_EQUAL);
			auto *parameterConflictScan =
					dynamic_cast<logical::LogicalNodeScan *>(parameterConflict->getChildren().front());
			ASSERT_NE(parameterConflictScan, nullptr);
			parameterConflictScan->setParameterPropertyPredicates({{"age", "age"}});
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(parameterConflict)).rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, PushesOneDirectLiteralRangeIntoNodeScanWithoutRemovingFilter) {
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{42}));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *filter = dynamic_cast<const logical::LogicalFilter *>(result.root.get());
			ASSERT_NE(filter, nullptr);
			EXPECT_EQ(filter->getPredicate(), predicate);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(filter->getChildren().front());
			ASSERT_NE(scan, nullptr);
			EXPECT_EQ(scan->getPreferredScanHint(), logical::LogicalScanHint::LSH_RANGE);
			ASSERT_EQ(scan->getRangePredicates().size(), 1U);
			EXPECT_EQ(scan->getRangePredicates().front().key, "age");
			EXPECT_EQ(scan->getRangePredicates().front().minValue, PropertyValue(int64_t{42}));
			EXPECT_EQ(scan->getRangePredicates().front().maxValue, PropertyValue());
			EXPECT_TRUE(scan->getRangePredicates().front().minInclusive);

			auto reverse = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n"),
					std::make_shared<expressions::BinaryOpExpression>(
							std::make_unique<expressions::LiteralExpression>(int64_t{42}),
							expressions::BinaryOperatorType::BOP_LESS_EQUAL,
							std::make_unique<expressions::VariableReferenceExpression>("n", "age")));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(reverse)).rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, CanonicalizesConjunctiveBoundsForOneRangeProperty) {
			auto lower = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER,
					std::make_unique<expressions::LiteralExpression>(int64_t{35}));
			auto looserLower = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{30}));
			auto upper = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_LESS_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{50}));
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::BinaryOpExpression>(
							std::move(lower), expressions::BinaryOperatorType::BOP_AND, std::move(looserLower)),
					expressions::BinaryOperatorType::BOP_AND, std::move(upper));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *filter = dynamic_cast<const logical::LogicalFilter *>(result.root.get());
			ASSERT_NE(filter, nullptr);
			EXPECT_EQ(filter->getPredicate(), predicate);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(filter->getChildren().front());
			ASSERT_NE(scan, nullptr);
			ASSERT_EQ(scan->getRangePredicates().size(), 1U);
			const logical::RangePredicate &range = scan->getRangePredicates().front();
			EXPECT_EQ(range.key, "age");
			EXPECT_EQ(range.minValue, PropertyValue(int64_t{35}));
			EXPECT_FALSE(range.minInclusive);
			EXPECT_EQ(range.maxValue, PropertyValue(int64_t{50}));
			EXPECT_TRUE(range.maxInclusive);

			auto mixedProperties = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n"),
					std::make_shared<expressions::BinaryOpExpression>(
							std::make_unique<expressions::BinaryOpExpression>(
									std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
									expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
									std::make_unique<expressions::LiteralExpression>(int64_t{30})),
								expressions::BinaryOperatorType::BOP_AND,
								std::make_unique<expressions::BinaryOpExpression>(
										std::make_unique<expressions::VariableReferenceExpression>("n", "score"),
										expressions::BinaryOperatorType::BOP_LESS,
										std::make_unique<expressions::LiteralExpression>(int64_t{50}))));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(mixedProperties)).rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, PrefersExclusiveBoundsWhenEqualRangeEndpointsAreConjoined) {
			auto inclusiveLower = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{35}));
			auto exclusiveLower = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER,
					std::make_unique<expressions::LiteralExpression>(int64_t{35}));
			auto inclusiveUpper = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_LESS_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{50}));
			auto exclusiveUpper = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_LESS,
					std::make_unique<expressions::LiteralExpression>(int64_t{50}));
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::BinaryOpExpression>(
							std::make_unique<expressions::BinaryOpExpression>(
									std::move(inclusiveLower), expressions::BinaryOperatorType::BOP_AND,
									std::move(exclusiveLower)),
							expressions::BinaryOperatorType::BOP_AND, std::move(inclusiveUpper)),
					expressions::BinaryOperatorType::BOP_AND, std::move(exclusiveUpper));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n", std::vector<std::string>{"Person"}), predicate);

			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *filter = dynamic_cast<const logical::LogicalFilter *>(result.root.get());
			ASSERT_NE(filter, nullptr);
			EXPECT_EQ(filter->getPredicate(), predicate);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(filter->getChildren().front());
			ASSERT_NE(scan, nullptr);
			ASSERT_EQ(scan->getRangePredicates().size(), 1U);
			const logical::RangePredicate &range = scan->getRangePredicates().front();
			EXPECT_EQ(range.minValue, PropertyValue(int64_t{35}));
			EXPECT_FALSE(range.minInclusive);
			EXPECT_EQ(range.maxValue, PropertyValue(int64_t{50}));
			EXPECT_FALSE(range.maxInclusive);
		}

		TEST(LogicalPlanOptimizerTest, ReplacesLooserRangeBoundsWithStricterEndpoints) {
			auto lower = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{10}));
			auto stricterLower = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_GREATER,
					std::make_unique<expressions::LiteralExpression>(int64_t{20}));
			auto upper = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_LESS_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{50}));
			auto stricterUpper = std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
					expressions::BinaryOperatorType::BOP_LESS,
					std::make_unique<expressions::LiteralExpression>(int64_t{40}));
			auto predicate = std::make_shared<expressions::BinaryOpExpression>(
					std::make_unique<expressions::BinaryOpExpression>(
							std::make_unique<expressions::BinaryOpExpression>(std::move(lower),
										expressions::BinaryOperatorType::BOP_AND, std::move(stricterLower)),
							expressions::BinaryOperatorType::BOP_AND, std::move(upper)),
					expressions::BinaryOperatorType::BOP_AND, std::move(stricterUpper));
			auto root = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n"), std::move(predicate));
			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root));
			ASSERT_NE(result.root, nullptr);
			const auto *filter = dynamic_cast<const logical::LogicalFilter *>(result.root.get());
			ASSERT_NE(filter, nullptr);
			const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(filter->getChildren().front());
			ASSERT_NE(scan, nullptr);
			ASSERT_EQ(scan->getRangePredicates().size(), 1U);
			EXPECT_EQ(scan->getRangePredicates().front().minValue, PropertyValue(int64_t{20}));
			EXPECT_EQ(scan->getRangePredicates().front().maxValue, PropertyValue(int64_t{40}));
		}

		TEST(LogicalPlanOptimizerTest, LeavesMalformedOrConflictingExistingScanPredicatesUntouched) {
			const auto makeEqualityFilter = [](std::unique_ptr<logical::LogicalNodeScan> scan) {
				return std::make_unique<logical::LogicalFilter>(
						std::move(scan),
						std::make_shared<expressions::BinaryOpExpression>(
								std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
								expressions::BinaryOperatorType::BOP_EQUAL,
								std::make_unique<expressions::LiteralExpression>(int64_t{42})));
			};

			auto duplicateProperties = std::make_unique<logical::LogicalNodeScan>(
					"n", std::vector<std::string>{"Person"},
					std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(int64_t{41})},
																								 {"age", PropertyValue(int64_t{40})}});
			const LogicalOptimizationResult duplicateResult =
					LogicalPlanOptimizer{}.optimize(makeEqualityFilter(std::move(duplicateProperties)));
			ASSERT_NE(duplicateResult.root, nullptr);
			EXPECT_EQ(duplicateResult.rewrite_count, 0U);
			const auto *duplicateFilter = dynamic_cast<const logical::LogicalFilter *>(duplicateResult.root.get());
			ASSERT_NE(duplicateFilter, nullptr);
			const auto *duplicateScan = dynamic_cast<const logical::LogicalNodeScan *>(duplicateFilter->getChildren().front());
			ASSERT_NE(duplicateScan, nullptr);
			EXPECT_EQ(duplicateScan->getPropertyPredicates().size(), 2U);
			EXPECT_EQ(duplicateScan->getPreferredScanHint(), logical::LogicalScanHint::LSH_FULL);

			auto emptyParameter = std::make_unique<logical::LogicalNodeScan>("n");
			emptyParameter->setParameterPropertyPredicates({{"", "age"}});
			const LogicalOptimizationResult parameterResult =
					LogicalPlanOptimizer{}.optimize(makeEqualityFilter(std::move(emptyParameter)));
			ASSERT_NE(parameterResult.root, nullptr);
			EXPECT_EQ(parameterResult.rewrite_count, 0U);
			const auto *parameterFilter = dynamic_cast<const logical::LogicalFilter *>(parameterResult.root.get());
			ASSERT_NE(parameterFilter, nullptr);
			const auto *parameterScan = dynamic_cast<const logical::LogicalNodeScan *>(parameterFilter->getChildren().front());
			ASSERT_NE(parameterScan, nullptr);
			EXPECT_EQ(parameterScan->getParameterPropertyPredicates().front().key, "");
			EXPECT_EQ(parameterScan->getPreferredScanHint(), logical::LogicalScanHint::LSH_FULL);

			auto conflictingScan = std::make_unique<logical::LogicalNodeScan>(
					"n", std::vector<std::string>{"Person"},
					std::vector<std::pair<std::string, PropertyValue>>{{"age", PropertyValue(int64_t{42})}});
			auto conflictingRange = std::make_unique<logical::LogicalFilter>(
					std::move(conflictingScan),
					std::make_shared<expressions::BinaryOpExpression>(
							std::make_unique<expressions::VariableReferenceExpression>("n", "age"),
							expressions::BinaryOperatorType::BOP_GREATER,
							std::make_unique<expressions::LiteralExpression>(int64_t{18})));
			const LogicalOptimizationResult rangeResult = LogicalPlanOptimizer{}.optimize(std::move(conflictingRange));
			ASSERT_NE(rangeResult.root, nullptr);
			EXPECT_EQ(rangeResult.rewrite_count, 0U);
			const auto *rangeFilter = dynamic_cast<const logical::LogicalFilter *>(rangeResult.root.get());
			ASSERT_NE(rangeFilter, nullptr);
			const auto *rangeScan = dynamic_cast<const logical::LogicalNodeScan *>(rangeFilter->getChildren().front());
			ASSERT_NE(rangeScan, nullptr);
			EXPECT_TRUE(rangeScan->getRangePredicates().empty());
			EXPECT_EQ(rangeScan->getPreferredScanHint(), logical::LogicalScanHint::LSH_FULL);
		}

		TEST(LogicalPlanOptimizerTest, PushesEveryNonNullLiteralRepresentationAndRejectsInvalidFilterShapes) {
			const auto makeFilter = [](std::unique_ptr<expressions::Expression> predicate,
									   std::string scanVariable = "n") {
				return std::make_unique<logical::LogicalFilter>(
						std::make_unique<logical::LogicalNodeScan>(std::move(scanVariable)),
						std::shared_ptr<expressions::Expression>(std::move(predicate)));
			};
			const auto makeEquality = [](std::unique_ptr<expressions::Expression> literal, std::string variable = "n") {
				return std::make_unique<expressions::BinaryOpExpression>(
						std::make_unique<expressions::VariableReferenceExpression>(std::move(variable), "key"),
						expressions::BinaryOperatorType::BOP_EQUAL, std::move(literal));
			};

			EXPECT_EQ(
					LogicalPlanOptimizer{}
							.optimize(makeFilter(makeEquality(std::make_unique<expressions::LiteralExpression>(true))))
							.rewrite_count,
					1U);
			EXPECT_EQ(
					LogicalPlanOptimizer{}
							.optimize(makeFilter(makeEquality(std::make_unique<expressions::LiteralExpression>(42.5))))
							.rewrite_count,
					1U);
			EXPECT_EQ(LogicalPlanOptimizer{}
							  .optimize(makeFilter(makeEquality(std::make_unique<expressions::LiteralExpression>(
									  PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t{7})})))))
							  .rewrite_count,
					  1U);

			auto nullPredicate = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalNodeScan>("n"), std::shared_ptr<expressions::Expression>{});
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nullPredicate)).rewrite_count, 0U);

			auto nonBinary = makeFilter(std::make_unique<expressions::LiteralExpression>(true));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nonBinary)).rewrite_count, 0U);

			auto variableMismatch =
					makeFilter(makeEquality(std::make_unique<expressions::LiteralExpression>(int64_t{7}), "other"));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(variableMismatch)).rewrite_count, 0U);

			auto nonScanChild = std::make_unique<logical::LogicalFilter>(
					std::make_unique<logical::LogicalLimit>(std::make_unique<logical::LogicalNodeScan>("n"), 5),
					std::shared_ptr<expressions::Expression>(
							makeEquality(std::make_unique<expressions::LiteralExpression>(int64_t{7}))));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nonScanChild)).rewrite_count, 0U);

			auto nullPropertyValue =
					makeFilter(makeEquality(std::make_unique<expressions::LiteralExpression>(PropertyValue())));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nullPropertyValue)).rewrite_count, 0U);

			for (const auto &invalidProperty:
				 std::vector<std::pair<std::string, std::string>>{{"", "key"}, {"n", ""}}) {
				auto invalid = makeFilter(std::make_unique<expressions::BinaryOpExpression>(
						std::make_unique<expressions::VariableReferenceExpression>(invalidProperty.first,
																				   invalidProperty.second),
						expressions::BinaryOperatorType::BOP_EQUAL,
						std::make_unique<expressions::LiteralExpression>(int64_t{7})));
				EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(invalid)).rewrite_count, 0U);
			}

			auto bareVariable = makeFilter(std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{7})));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(bareVariable)).rewrite_count, 0U);

			auto nonLiteral = makeFilter(std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "key"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::VariableReferenceExpression>("value")));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nonLiteral)).rewrite_count, 0U);

			auto nullOperand = makeFilter(std::make_unique<expressions::BinaryOpExpression>(
					std::unique_ptr<expressions::Expression>{}, expressions::BinaryOperatorType::BOP_EQUAL,
					std::make_unique<expressions::LiteralExpression>(int64_t{7})));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nullOperand)).rewrite_count, 0U);

			auto declaredNodeScan = std::make_unique<logical::LogicalFilter>(
					std::make_unique<DeclaredNodeScanWithoutNodeScanImplementation>(),
					std::shared_ptr<expressions::Expression>(
							makeEquality(std::make_unique<expressions::LiteralExpression>(int64_t{7}))));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(declaredNodeScan)).rewrite_count, 0U);

			auto emptyVariable = makeFilter(makeEquality(std::make_unique<expressions::LiteralExpression>(int64_t{7})), "");
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(emptyVariable)).rewrite_count, 0U);

			auto nullRightOperand = makeFilter(std::make_unique<expressions::BinaryOpExpression>(
					std::make_unique<expressions::VariableReferenceExpression>("n", "key"),
					expressions::BinaryOperatorType::BOP_EQUAL,
					std::unique_ptr<expressions::Expression>{}));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(nullRightOperand)).rewrite_count, 0U);

			// The optimizer validates the child list during traversal and re-checks it
			// before applying a rewrite. A transient malformed view must fail closed.
			auto transientPredicate = makeEquality(std::make_unique<expressions::LiteralExpression>(int64_t{7}));
			auto transient = std::make_unique<LogicalFilterWithTransientNullChild>(
					std::make_unique<logical::LogicalNodeScan>("n"),
					std::shared_ptr<expressions::Expression>(std::move(transientPredicate)));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(transient)).rewrite_count, 0U);

			auto compositeRangeScan = std::make_unique<logical::LogicalNodeScan>("n");
			compositeRangeScan->setCompositeEquality({{"key"}, {PropertyValue(int64_t{1})}});
			auto compositeRange = std::make_unique<logical::LogicalFilter>(
					std::move(compositeRangeScan),
					std::make_shared<expressions::BinaryOpExpression>(
							std::make_unique<expressions::VariableReferenceExpression>("n", "key"),
							expressions::BinaryOperatorType::BOP_GREATER,
							std::make_unique<expressions::LiteralExpression>(int64_t{7})));
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(compositeRange)).rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, LeavesMalformedOrOverflowingWindowsForRuntimeValidation) {
			auto malformed = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalLimit>(std::make_unique<logical::LogicalNodeScan>("node"), 3), -1);
			const LogicalOptimizationResult malformedResult = LogicalPlanOptimizer{}.optimize(std::move(malformed));
			ASSERT_NE(malformedResult.root, nullptr);
			EXPECT_EQ(malformedResult.rewrite_count, 0U);
			EXPECT_EQ(malformedResult.root->getType(), logical::LogicalOpType::LOP_LIMIT);
			EXPECT_EQ(malformedResult.root->getChildren().front()->getType(), logical::LogicalOpType::LOP_LIMIT);
			auto malformedInner = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalLimit>(std::make_unique<logical::LogicalNodeScan>("node"), -1), 3);
			const LogicalOptimizationResult malformedInnerResult =
					LogicalPlanOptimizer{}.optimize(std::move(malformedInner));
			EXPECT_EQ(malformedInnerResult.rewrite_count, 0U);

			auto zeroLimit = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"), 1), 0);
			const LogicalOptimizationResult zeroLimitResult = LogicalPlanOptimizer{}.optimize(std::move(zeroLimit));
			ASSERT_NE(zeroLimitResult.root, nullptr);
			EXPECT_EQ(zeroLimitResult.rewrite_count, 0U);
			EXPECT_EQ(zeroLimitResult.root->getType(), logical::LogicalOpType::LOP_LIMIT);
			auto negativeSkip = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"), -1), 1);
			const LogicalOptimizationResult negativeSkipResult = LogicalPlanOptimizer{}.optimize(std::move(negativeSkip));
			ASSERT_NE(negativeSkipResult.root, nullptr);
			EXPECT_EQ(negativeSkipResult.rewrite_count, 0U);
			EXPECT_EQ(negativeSkipResult.root->getType(), logical::LogicalOpType::LOP_LIMIT);

			auto overflowLimitSkip = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"),
																std::numeric_limits<int64_t>::max()),
					1);
			const LogicalOptimizationResult overflowLimitSkipResult = LogicalPlanOptimizer{}.optimize(std::move(overflowLimitSkip));
			ASSERT_NE(overflowLimitSkipResult.root, nullptr);
			EXPECT_EQ(overflowLimitSkipResult.rewrite_count, 0U);
			EXPECT_EQ(overflowLimitSkipResult.root->getType(), logical::LogicalOpType::LOP_LIMIT);

			auto overflowing = std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"), 1),
					std::numeric_limits<int64_t>::max());
			const LogicalOptimizationResult overflowingResult = LogicalPlanOptimizer{}.optimize(std::move(overflowing));
			ASSERT_NE(overflowingResult.root, nullptr);
			EXPECT_EQ(overflowingResult.rewrite_count, 0U);
			EXPECT_EQ(overflowingResult.root->getChildren().front()->getType(), logical::LogicalOpType::LOP_SKIP);
			auto malformedSkipOuter = std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"), 1), -1);
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(malformedSkipOuter)).rewrite_count, 0U);
			auto malformedSkipInner = std::make_unique<logical::LogicalSkip>(
					std::make_unique<logical::LogicalSkip>(std::make_unique<logical::LogicalNodeScan>("node"), -1), 1);
			EXPECT_EQ(LogicalPlanOptimizer{}.optimize(std::move(malformedSkipInner)).rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, ValidatesStatisticsAndPlanTraversalBounds) {
			FixedStatisticsProvider valid({.lower_bound = 1U, .expected_rows = 3U, .upper_bound = 9U});
			auto validResult =
					LogicalPlanOptimizer{}.optimize(std::make_unique<logical::LogicalNodeScan>("node"), &valid);
			ASSERT_TRUE(validResult.root_cardinality.has_value());
			EXPECT_EQ(validResult.root_cardinality->expected_rows, 3U);

			FixedStatisticsProvider invalid({.lower_bound = 3U, .expected_rows = 1U, .upper_bound = 9U});
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(
								std::make_unique<logical::LogicalNodeScan>("node"), &invalid);
						static_cast<void>(ignored);
					},
					std::invalid_argument);

			LogicalOptimizerOptions options;
			options.maximum_plan_depth = 2U;
			auto tooDeep = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalLimit>(std::make_unique<logical::LogicalNodeScan>("node"), 2), 1);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{options}.optimize(std::move(tooDeep));
						static_cast<void>(ignored);
					},
					std::length_error);
		}

		TEST(LogicalPlanOptimizerTest, PlacesTheSmallerCartesianInputOnTheMaterializedRightSide) {
			VariableStatisticsProvider statistics;
			auto root = std::make_unique<logical::LogicalJoin>(
					std::make_unique<logical::LogicalNodeScan>("small"),
					std::make_unique<logical::LogicalNodeScan>("large"));
			const LogicalOptimizationResult result = LogicalPlanOptimizer{}.optimize(std::move(root), &statistics);
			ASSERT_NE(result.root, nullptr);
			EXPECT_EQ(result.rewrite_count, 1U);
			const auto *join = dynamic_cast<const logical::LogicalJoin *>(result.root.get());
			ASSERT_NE(join, nullptr);
			const auto *left = dynamic_cast<const logical::LogicalNodeScan *>(join->getLeft());
			const auto *right = dynamic_cast<const logical::LogicalNodeScan *>(join->getRight());
			ASSERT_NE(left, nullptr);
			ASSERT_NE(right, nullptr);
			EXPECT_EQ(left->getVariable(), "large");
			EXPECT_EQ(right->getVariable(), "small");

			FixedStatisticsProvider tied({.lower_bound = 0U, .expected_rows = 5U, .upper_bound = 5U});
			auto stable = std::make_unique<logical::LogicalJoin>(
					std::make_unique<logical::LogicalNodeScan>("small"),
					std::make_unique<logical::LogicalNodeScan>("large"));
			const LogicalOptimizationResult stableResult = LogicalPlanOptimizer{}.optimize(std::move(stable), &tied);
			EXPECT_EQ(stableResult.rewrite_count, 0U);
		}

		TEST(LogicalPlanOptimizerTest, HandlesEmptyInputsAndInvalidConfiguration) {
			const LogicalOptimizationResult empty = LogicalPlanOptimizer{}.optimize(nullptr);
			EXPECT_EQ(empty.root, nullptr);
			EXPECT_EQ(empty.rewrite_count, 0U);

			NoEstimateStatisticsProvider noEstimate;
			const LogicalOptimizationResult noEstimateResult =
					LogicalPlanOptimizer{}.optimize(std::make_unique<logical::LogicalNodeScan>("node"), &noEstimate);
			EXPECT_FALSE(noEstimateResult.root_cardinality.has_value());

			LogicalOptimizerOptions zeroPasses;
			zeroPasses.maximum_rewrite_passes = 0U;
			EXPECT_THROW(LogicalPlanOptimizer{zeroPasses}, std::invalid_argument);
			LogicalOptimizerOptions zeroDepth;
			zeroDepth.maximum_plan_depth = 0U;
			EXPECT_THROW(LogicalPlanOptimizer{zeroDepth}, std::invalid_argument);
		}

		TEST(LogicalPlanOptimizerTest, FailsClosedForMalformedOperatorContracts) {
			EXPECT_THROW(
					{
						auto ignored =
								LogicalPlanOptimizer{}.optimize(std::make_unique<logical::LogicalLimit>(nullptr, 1));
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(std::make_unique<BrokenLogicalLimit>(0U));
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(
								std::make_unique<DeclaredLimitWithoutLimitImplementation>());
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(
								std::make_unique<DeclaredSkipWithoutSkipImplementation>());
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(
								std::make_unique<DeclaredFilterWithoutFilterImplementation>());
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(std::make_unique<BrokenLogicalLimit>(1U));
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto root =
								std::make_unique<logical::LogicalLimit>(std::make_unique<BrokenLogicalLimit>(2U), 1);
						auto ignored = LogicalPlanOptimizer{}.optimize(std::move(root));
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{}.optimize(std::make_unique<BrokenLogicalSkip>(1U));
						static_cast<void>(ignored);
					},
					std::logic_error);
			EXPECT_THROW(
					{
						auto root = std::make_unique<logical::LogicalSkip>(std::make_unique<BrokenLogicalSkip>(2U), 1);
						auto ignored = LogicalPlanOptimizer{}.optimize(std::move(root));
						static_cast<void>(ignored);
					},
					std::logic_error);
		}

		TEST(LogicalPlanOptimizerTest, EnforcesItsFixedPointBound) {
			LogicalOptimizerOptions options;
			options.maximum_rewrite_passes = 1U;
			auto root = std::make_unique<logical::LogicalLimit>(
					std::make_unique<logical::LogicalLimit>(std::make_unique<logical::LogicalNodeScan>("node"), 2), 1);
			EXPECT_THROW(
					{
						auto ignored = LogicalPlanOptimizer{options}.optimize(std::move(root));
						static_cast<void>(ignored);
					},
					std::logic_error);
		}

	} // namespace
} // namespace graph::query::optimization
