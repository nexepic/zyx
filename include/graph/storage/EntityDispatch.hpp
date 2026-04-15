/**
 * @file EntityDispatch.hpp
 * @brief Compile-time and runtime entity type dispatch utilities
 *
 * Provides EntitySizeTraits for compile-time type->size mapping and
 * dispatchByType() for runtime type-erased dispatch, eliminating
 * repetitive switch-case blocks across the storage layer.
 *
 * @copyright Copyright (c) 2025 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <stdexcept>
#include "graph/core/Types.hpp"
#include "graph/core/Blob.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Index.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Property.hpp"
#include "graph/core/State.hpp"
#include "SegmentType.hpp"

namespace graph::storage {

	template<EntityType ET>
	struct EntitySizeTraits;

	template<>
	struct EntitySizeTraits<EntityType::Node> {
		using Type = Node;
		static constexpr size_t entitySize = Node::getTotalSize();
	};

	template<>
	struct EntitySizeTraits<EntityType::Edge> {
		using Type = Edge;
		static constexpr size_t entitySize = Edge::getTotalSize();
	};

	template<>
	struct EntitySizeTraits<EntityType::Property> {
		using Type = Property;
		static constexpr size_t entitySize = Property::getTotalSize();
	};

	template<>
	struct EntitySizeTraits<EntityType::Blob> {
		using Type = Blob;
		static constexpr size_t entitySize = Blob::getTotalSize();
	};

	template<>
	struct EntitySizeTraits<EntityType::Index> {
		using Type = Index;
		static constexpr size_t entitySize = Index::getTotalSize();
	};

	template<>
	struct EntitySizeTraits<EntityType::State> {
		using Type = State;
		static constexpr size_t entitySize = State::getTotalSize();
	};

	/// Runtime dispatch: invoke a generic lambda with the correct EntityType template argument.
	/// The callable F must be a generic lambda: []<EntityType ET>() { ... }
	template<typename F>
	decltype(auto) dispatchByType(uint32_t type, F &&func) {
		switch (type) {
			case toUnderlying(EntityType::Node):
				return func.template operator()<EntityType::Node>();
			case toUnderlying(EntityType::Edge):
				return func.template operator()<EntityType::Edge>();
			case toUnderlying(EntityType::Property):
				return func.template operator()<EntityType::Property>();
			case toUnderlying(EntityType::Blob):
				return func.template operator()<EntityType::Blob>();
			case toUnderlying(EntityType::Index):
				return func.template operator()<EntityType::Index>();
			case toUnderlying(EntityType::State):
				return func.template operator()<EntityType::State>();
			default:
				throw std::invalid_argument("Invalid entity type");
		}
	}

	/// Convenience: get entity size at runtime without a lambda.
	inline size_t getEntitySize(uint32_t type) {
		return dispatchByType(type, []<EntityType ET>() -> size_t {
			return EntitySizeTraits<ET>::entitySize;
		});
	}

} // namespace graph::storage
