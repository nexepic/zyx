#include <charconv>
#include <filesystem>
#include <iostream>
#include <string_view>

#include "graph/storage/relocatable/format/ScanBuildingImageBuilder.hpp"
#include "query/execution/ProcessResourceDomains.hpp"

// This explicit offline operation never replaces its source or an existing
// destination. The builder alone may publish a complete destination, after
// integrity and actual whole-file space admission.
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: zyx-scan-convert SOURCE DESTINATION [--field-cohort 1..8]\n"
                     "Field cohorts share pages across logical groups; default 1 preserves group-local placement.\n"
                     "Close all users of SOURCE before conversion. DESTINATION must not exist.\n"
                     "A failed conversion may leave an incomplete BUILDING destination.\n";
        return 0;
    }
    if (argc != 3 && argc != 5) {
        std::cerr << "Usage: zyx-scan-convert SOURCE DESTINATION [--field-cohort 1..8]\n";
        return 2;
    }
    try {
        using namespace graph::storage;
        unsigned cohort = 1;
        if (argc == 5) {
            const std::string_view option(argv[3]), value(argv[4]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), cohort);
            if (option != "--field-cohort" || parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || !cohort || cohort > 8U)
                throw std::invalid_argument("--field-cohort requires an integer from 1 to 8");
        }
        constexpr size_t mib = 1024U * 1024U;
        auto source = std::make_shared<graph::query::execution::MaterializedByteBudget>(mib);
        auto work = std::make_shared<graph::query::execution::MaterializedByteBudget>(7U * mib);
        auto process = graph::query::execution::detail::ProcessResourceDomains::defaultOwner();
        const auto domain = process->domain("offline-scan:" + std::filesystem::absolute(argv[2]).generic_string());
        const auto cache = process->cacheGrant(domain);
        auto builder = ScanBuildingImageBuilder::cloneWithPrimaryMembership(
            argv[1], argv[2], source, UINT64_C(2) * 1024U * mib, 989, nullptr, cache);
        const auto fields = builder->constructFields(nullptr, work, static_cast<uint8_t>(cohort));
        std::cerr << "examined_groups=" << fields.examined_groups
                  << " converted_groups=" << fields.converted_groups
                  << " row_fallback_groups=" << fields.row_fallback_groups << '\n';
        for (;;) {
            const auto retired = builder->releaseRetiredPoolOnce(work);
            if (retired.blocked) throw std::runtime_error("retired source pool remains pinned");
            if (retired.released) continue;
            const auto before = builder->buildingRoot();
            if (builder->reclaimMetadataOnce(work).superblock != before) continue;
            if (builder->compactDataPoolOnce(work)) continue;
            if (builder->compactMetadataPoolOnce(work)) continue;
            static_cast<void>(builder->truncateReleasedTailOnce(work));
            const auto ready = builder->complete(work);
            std::cout << "generation=" << ready.generation.value() << '\n';
            return 0;
        }
    } catch (const std::exception &error) {
        std::cerr << "Conversion failed: " << error.what() << '\n';
        return 1;
    }
}
