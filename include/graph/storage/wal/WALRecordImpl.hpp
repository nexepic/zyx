/**
 * @file WALRecordImpl.hpp
 * @author Nexepic
 * @date 2025/3/24
 *
 * @copyright Copyright (c) 2025 Nexepic
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

#include "WALRecord.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"

namespace graph::storage::wal {

	class NodeRecord : public WALRecord {
	public:
		NodeRecord(WALRecordType type, const graph::Node &node, uint64_t txnId = 0) :
			WALRecord(type, txnId), node_(node) {}
		NodeRecord(WALRecordType type) : WALRecord(type) {}

		size_t getSize() const override;
		void serialize(uint8_t *buffer) const override;
		void deserialize(std::istream &in) override;

		const graph::Node &getNode() const { return node_; }

		static std::unique_ptr<NodeRecord> deserialize(const WALRecordHeader &header, const uint8_t *data);

		// Convert to JSON for debugging
		[[nodiscard]] std::string toJSON() const;

	private:
		graph::Node node_;
	};

	class EdgeRecord : public WALRecord {
	public:
		EdgeRecord(WALRecordType type, const graph::Edge &edge, uint64_t txnId = 0) :
			WALRecord(type, txnId), edge_(edge) {}
		EdgeRecord(WALRecordType type) : WALRecord(type) {}

		size_t getSize() const override;
		void serialize(uint8_t *buffer) const override;
		void deserialize(std::istream &in) override;

		const graph::Edge &getEdge() const { return edge_; }

		static std::unique_ptr<EdgeRecord> deserialize(const WALRecordHeader &header, const uint8_t *data);

		// Convert to JSON for debugging
		[[nodiscard]] std::string toJSON() const;

	private:
		graph::Edge edge_;
	};

	class TransactionRecord : public WALRecord {
	public:
		TransactionRecord(WALRecordType type, uint64_t txnId) : WALRecord(type, txnId) {}
		TransactionRecord(WALRecordType type) : WALRecord(type) {}

		size_t getSize() const override;
		void serialize(uint8_t *buffer) const override;
		void deserialize(std::istream &in) override;

		static std::unique_ptr<TransactionRecord> deserialize(const WALRecordHeader &header, const uint8_t *data);

		// Convert to JSON for debugging
		[[nodiscard]] std::string toJSON() const;
	};

	class CheckpointRecord : public WALRecord {
	public:
		CheckpointRecord() : WALRecord(WALRecordType::CHECKPOINT) {}

		size_t getSize() const override;
		void serialize(uint8_t *buffer) const override;
		void deserialize(std::istream &in) override;

		// Convert to JSON for debugging
		[[nodiscard]] std::string toJSON() const;

	private:
		uint64_t lastAppliedLSN;
	};

} // namespace graph::storage::wal
