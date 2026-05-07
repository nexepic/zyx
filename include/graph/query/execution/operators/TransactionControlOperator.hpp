/**
 * @file TransactionControlOperator.hpp
 * @date 2026/3/26
 *
 * @copyright Copyright (c) 2026 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include "../PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	enum class TransactionCommand { TXN_CTL_BEGIN, TXN_CTL_COMMIT, TXN_CTL_ROLLBACK };

	class TransactionControlOperator : public PhysicalOperator {
	public:
		explicit TransactionControlOperator(TransactionCommand cmd) : command_(cmd) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			std::string statusMsg;
			if (command_ == TransactionCommand::TXN_CTL_BEGIN) statusMsg = "Transaction started";
			else if (command_ == TransactionCommand::TXN_CTL_COMMIT) statusMsg = "Transaction committed";
			else statusMsg = "Transaction rolled back";

			Record record;
			record.setValue("result", PropertyValue(statusMsg));
			RecordBatch batch;
			batch.push_back(std::move(record));
			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] TransactionCommand getCommand() const { return command_; }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"result"}; }

		[[nodiscard]] std::string toString() const override {
			if (command_ == TransactionCommand::TXN_CTL_BEGIN) return "TransactionControl(BEGIN)";
			if (command_ == TransactionCommand::TXN_CTL_COMMIT) return "TransactionControl(COMMIT)";
			return "TransactionControl(ROLLBACK)";
		}

	private:
		TransactionCommand command_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
