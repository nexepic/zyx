// Included inside the isolated worker namespace; shares its allocator probe.
template<class Definition, class Codec>
void ownedSchemaDefinitionHeapAccountingCase(const std::string &mode) {
    using namespace graph::storage;
    size_t cases = 0U;
    for (const size_t length : {1U, 15U, 16U, 31U, 1024U, 4096U}) {
        Definition prototype;
        prototype.name = std::string(length, 'n');
        if constexpr (std::is_same_v<Definition, IndexDefinition>) {
            prototype.index_id = 1U;
            prototype.kind = length == 4096U ? IndexDefinitionKind::IDK_COMPOSITE : IndexDefinitionKind::IDK_PROPERTY;
            prototype.label_token_id = 1U;
        } else {
            prototype.constraint_id = RSS_SYSTEM_CATALOG_CONSTRAINT_ID_FIRST;
            prototype.kind = length == 4096U ? ConstraintDefinitionKind::CDK_NODE_KEY : ConstraintDefinitionKind::CDK_UNIQUE;
            prototype.schema_token_id = 1U;
        }
        const size_t properties = length == 4096U ? 64U : 1U;
        for (size_t index = 0U; index < properties; ++index)
            prototype.property_names.emplace_back(std::string(length == 4096U ? 1000U : 16U, 'p') + std::to_string(index));
        { RejectAllocations noHeap; Codec::validate(prototype); }
        const auto encoded = Codec::encode(prototype);
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto baseline = liveBytes.load(std::memory_order_relaxed);
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        std::optional<Definition> actual;
        try {
            graph::utils::OwnedAllocationScope scope(memory);
            auto serialized = Codec::encodeOwned(prototype);
            if (!std::ranges::equal(serialized, encoded)) throw std::runtime_error("owned schema encoding changed wire bytes");
            actual.emplace(Codec::decode(encoded));
            {
                graph::utils::OwnedStdVector<Definition> values;
                values.push_back(*actual); values.reserve(8U);
                graph::utils::OwnedStdVector<Definition> copied(values);
            }
            observeWriteOwnership(liveBytes.load(std::memory_order_relaxed));
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (*actual != prototype) throw std::runtime_error("owned index definition changed decoded content");
        actual.reset();
        if (maximumUncoveredWriteBytes != 0U)
            throw std::runtime_error("owned index definition underestimated requests: " + std::to_string(maximumUncoveredWriteBytes));
        if (memory->used() != 0U || liveBytes.load(std::memory_order_relaxed) != baseline)
            throw std::runtime_error("owned index definition retained credit or storage");
        {
            using namespace graph::query::execution::detail;
            const std::vector<Definition> input{prototype};
            const std::vector<Definition> empty;
            constexpr size_t capacity = 4U * 1024U * 1024U;
            auto process = std::make_shared<ProcessResourceBudget>(ResourceLimits::uniform(capacity));
            auto database = std::make_shared<DatabaseResourceBudget>(process, ResourceLimits::uniform(capacity));
            auto account = std::make_shared<ResourceAccount>(database, ResourceClass::RCK_CACHE, capacity);
            auto cache = std::make_shared<PrepaidMemoryResource>(account, capacity);
            const auto cacheBaseline = liveBytes.load(std::memory_order_relaxed);
            observedInitialLive = cacheBaseline; observedInitialCharge = 0U;
            maximumUncoveredWriteBytes = 0U; observedPrepaidBudget = cache.get();
            try {
                {
                    NativeReadSession session(cache);
                    if constexpr (std::is_same_v<Definition, IndexDefinition>) {
                        session.putIndexDefinitions(input);
                        if (session.indexDefinitions(64U) == nullptr) throw std::runtime_error("owned index cache bypassed a valid shape");
                        session.putIndexDefinitions(empty);
                    } else {
                        session.putConstraintDefinitions(input);
                        if (session.constraintDefinitions(64U) == nullptr) throw std::runtime_error("owned constraint cache bypassed a valid shape");
                        session.putConstraintDefinitions(empty);
                    }
                }
            } catch (...) { observedPrepaidBudget = nullptr; throw; }
            observedPrepaidBudget = nullptr;
            if (maximumUncoveredWriteBytes != 0U)
                throw std::runtime_error("schema cache underestimated requests: " + std::to_string(maximumUncoveredWriteBytes));
            if (cache->used() != 0U || liveBytes.load(std::memory_order_relaxed) != cacheBaseline)
                throw std::runtime_error("schema cache retained credit or storage");
        }
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}

void ownedCatalogKeyHeapAccountingCase(const std::string &mode) {
    size_t cases = 0U;
    for (const size_t length : {1U, 16U, RSS_SYSTEM_CATALOG_NAME_MAX_BYTES - 1U, RSS_SYSTEM_CATALOG_NAME_MAX_BYTES}) {
        const std::string name(length, 'n');
        constexpr auto label = SystemCatalogSchemaTokenKind::SCSTK_LABEL;
        constexpr auto type = SystemCatalogSchemaTokenKind::SCSTK_RELATIONSHIP_TYPE;
        const std::array<std::vector<std::byte>, 13U> expected{
            SystemCatalogTokenKeyCodec::bootstrapKey(), SystemCatalogTokenKeyCodec::propertyNameKey(name),
            SystemCatalogTokenKeyCodec::propertyTokenKey(1U, name), SystemCatalogTokenKeyCodec::schemaNameKey(label, name),
            SystemCatalogTokenKeyCodec::schemaTokenKey(label, 1U, name), SystemCatalogTokenKeyCodec::propertyTokenSequenceKey(),
            SystemCatalogTokenKeyCodec::schemaTokenSequenceKey(), SystemCatalogGraphCardinalityCodec::nodeCountKey(),
            SystemCatalogGraphCardinalityCodec::edgeCountKey(), SystemCatalogTokenCardinalityCodec::countKey(label, 1U),
            SystemCatalogTokenCardinalityCodec::completeKey(label), SystemCatalogTokenCardinalityCodec::countKey(type, 0U),
            SystemCatalogTokenCardinalityCodec::completeKey(type)};
        auto memory = std::make_shared<graph::query::execution::MaterializedByteBudget>(4U * 1024U * 1024U);
        const auto baseline = liveBytes.load();
        observedInitialLive = baseline; observedInitialCharge = 0U;
        maximumUncoveredWriteBytes = 0U; observedWriteBudget = memory.get();
        try {
            graph::utils::OwnedAllocationScope scope(memory);
            graph::utils::OwnedStdVector<graph::utils::OwnedStdVector<std::byte>> keys;
            keys.reserve(expected.size());
            keys.push_back(SystemCatalogTokenKeyCodec::bootstrapKeyOwned());
            keys.push_back(SystemCatalogTokenKeyCodec::propertyNameKeyOwned(name));
            keys.push_back(SystemCatalogTokenKeyCodec::propertyTokenKeyOwned(1U, name));
            keys.push_back(SystemCatalogTokenKeyCodec::schemaNameKeyOwned(label, name));
            keys.push_back(SystemCatalogTokenKeyCodec::schemaTokenKeyOwned(label, 1U, name));
            keys.push_back(SystemCatalogTokenKeyCodec::propertyTokenSequenceKeyOwned());
            keys.push_back(SystemCatalogTokenKeyCodec::schemaTokenSequenceKeyOwned());
            keys.push_back(SystemCatalogGraphCardinalityCodec::nodeCountKeyOwned());
            keys.push_back(SystemCatalogGraphCardinalityCodec::edgeCountKeyOwned());
            keys.push_back(SystemCatalogTokenCardinalityCodec::countKeyOwned(label, 1U));
            keys.push_back(SystemCatalogTokenCardinalityCodec::completeKeyOwned(label));
            keys.push_back(SystemCatalogTokenCardinalityCodec::countKeyOwned(type, 0U));
            keys.push_back(SystemCatalogTokenCardinalityCodec::completeKeyOwned(type));
            for (size_t i = 0U; i < keys.size(); ++i)
                if (!std::ranges::equal(keys[i], expected[i])) throw std::runtime_error("owned catalog key changed wire bytes");
            auto copied = keys; copied.reserve(32U);
            observeWriteOwnership(liveBytes.load());
        } catch (...) { observedWriteBudget = nullptr; throw; }
        observedWriteBudget = nullptr;
        if (maximumUncoveredWriteBytes != 0U || memory->used() != 0U || liveBytes.load() != baseline)
            throw std::runtime_error("catalog key copy lost heap ownership: " + std::to_string(maximumUncoveredWriteBytes));
        auto small = std::make_shared<graph::query::execution::MaterializedByteBudget>(1024U);
        if (length == RSS_SYSTEM_CATALOG_NAME_MAX_BYTES) {
            bool rejected = false;
            try {
                graph::utils::OwnedAllocationScope scope(small);
                (void)SystemCatalogTokenKeyCodec::propertyNameKeyOwned(name);
            } catch (const graph::query::execution::MaterializedByteLimitExceeded &) { rejected = true; }
            if (!rejected || small->used() != 0U) throw std::runtime_error("catalog key rejection retained credit");
        }
        ++cases;
    }
    std::cout << "{\"mode\":\"" << mode << "\",\"shape_cases\":" << cases
        << ",\"key_layouts\":13,\"maximum_uncovered_private_bytes\":0,\"passed\":true}\n";
}
