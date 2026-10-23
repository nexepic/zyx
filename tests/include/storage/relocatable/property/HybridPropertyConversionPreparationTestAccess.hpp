#pragma once
#include "graph/storage/relocatable/metadata/HybridPropertyConversionPreparation.hpp"
namespace graph::storage {
class HybridPropertyConversionPreparationTestAccess final {
public:
    using WholeRootResolver = HybridPropertyConversionPreparation::WholeRootResolver;
    static auto prepareCold(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}, const WholeRootResolver &wholeRoot = {}) {
        return std::unique_ptr<HybridPropertyConversionPreparation>(new HybridPropertyConversionPreparation(
            batch, plan, checkpoint, {}, true, true, wholeRoot));
    }
    static auto prepare(MetadataWriteBatch &batch, const HybridPropertyGroupConversion &plan,
        const std::function<void()> &checkpoint = {}) {
        return std::unique_ptr<HybridPropertyConversionPreparation>(new HybridPropertyConversionPreparation(batch, plan, checkpoint));
    }
};
}
