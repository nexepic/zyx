// Apply the existing derived-index maintenance operation to a fresh private
// copy of an admitted image. This is not a converter or a query benchmark.
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
int main(int argc,char **argv) {
    if(argc!=4)return 2;
    try {
        namespace fs=std::filesystem;
        using namespace graph::storage;
        const fs::path source=argv[1],target=argv[2],rowSource=argv[3];
        constexpr uint64_t total=UINT64_C(2)*1024U*1024U*1024U;
        const uint64_t sourceBytes=fs::file_size(source),rowBytes=fs::file_size(rowSource);
        if(fs::exists(target)||rowBytes>total||sourceBytes>total-rowBytes||sourceBytes>total-rowBytes-sourceBytes)
            throw std::runtime_error("fresh derived maintenance copy exceeds the original aggregate file limit");
        fs::copy_file(source,target);
        RelocatableStorageBackendOptions options;
        options.data_path=target;options.open_mode=OpenMode::OPEN_EXISTING_FILE;
        options.buffer_pool_bytes=64U*1024U*1024U;
        options.maximum_database_file_bytes=total-rowBytes-sourceBytes;
        options.maximum_integrity_workspace_bytes=7U*1024U*1024U;
        options.maximum_reclaim_workspace_bytes=7U*1024U*1024U;
        auto workspace=std::make_shared<graph::query::execution::MaterializedByteBudget>(7U*1024U*1024U);
        RelocatableStorageBackend backend(options);backend.open();
        const auto before=backend.openSnapshot().superblock();
        backend.rebuildNativeEntityFreeSpaceSummaries({},workspace);
        const auto after=backend.openSnapshot().superblock();
        if(after.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING)throw std::runtime_error("maintenance changed completion state");
        backend.close();
        const uint64_t bytes=fs::file_size(target);
        if(workspace->used()||bytes>total-rowBytes-sourceBytes||bytes*10U>rowBytes*9U)
            throw std::runtime_error("derived maintenance did not retain resource/whole-file admission");
        std::cout<<"{\"query_benchmark\":false,\"before_generation\":"<<before.generation.value()
                 <<",\"after_generation\":"<<after.generation.value()<<",\"source_ready_bytes\":"<<sourceBytes
                 <<",\"target_bytes\":"<<bytes<<",\"row_source_bytes\":"<<rowBytes
                 <<",\"node_directory_unchanged\":"<<(before.node_entity_directory_root==after.node_entity_directory_root?"true":"false")
                 <<",\"edge_directory_unchanged\":"<<(before.edge_entity_directory_root==after.edge_entity_directory_root?"true":"false")
                 <<",\"maintenance_account_used\":"<<workspace->used()<<"}\n";
        return 0;
    }catch(const std::exception &error){std::cerr<<"derived_maintenance_error="<<error.what()<<'\n';return 1;}
}
