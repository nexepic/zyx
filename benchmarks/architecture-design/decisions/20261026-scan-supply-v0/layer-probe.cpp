// Diagnostic only: real certified storage supply with a bounded scalar consumer.
#include <array>
#include <bit>
#include <chrono>
#include <iostream>
#include <string>
#include "graph/storage/relocatable/backend/RelocatableStorageBackend.hpp"
#include "graph/storage/LogicalGraphNodeScan.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
using namespace graph;
using namespace graph::storage;
using Clock = std::chrono::steady_clock;
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try {
        RelocatableStorageBackendOptions options;
        options.data_path = argv[1]; options.open_mode = OpenMode::OPEN_READ_ONLY_EXISTING_FILE;
        auto backend = std::make_shared<RelocatableStorageBackend>(options); backend->open();
        for (bool text : {false, true}) for (int iteration = -3; iteration < 5; ++iteration) {
            auto snapshot = backend->openLogicalGraphReadSnapshot();
            auto *provider = dynamic_cast<const LogicalGraphNodeScanProvider *>(snapshot.get());
            if (!provider) throw std::runtime_error("no scan provider");
            auto memory = std::make_shared<query::execution::MaterializedByteBudget>(64U*1024U*1024U);
            utils::OwnedAllocationScope scope(memory);
            query::execution::MaterializedByteReservation sourceCredit(memory.get()), batchCredit(memory.get());
            size_t examined = 0, selected = 0, values = 0, matches = 0, calls = 0, batches = 0;
            double sourceMs = 0;
            const std::string_view key = text ? "country" : "score";
            const std::vector<std::string> labels = text ? std::vector<std::string>{"User"} : std::vector<std::string>{};
            const auto begin = Clock::now();
            const auto completed = provider->tryVisitNodeScanBatches(SIZE_MAX,512,labels,std::span(&key,1),
                [&](std::span<const uint64_t> ids,size_t count,const auto &read) {
                    ++batches; examined += count; selected += ids.size(); sourceCredit.reset();
                    std::array<bool,512> seen{}, matched{};
                    const auto set = [&](size_t row, bool match) {
                        if (row>=ids.size() || seen[row]) throw std::runtime_error("duplicate/out-of-range value");
                        seen[row] = true; matched[row] = match; ++values;
                    };
                    const LogicalGraphReadSnapshot::PropertyCellSink owning = [&](size_t row,size_t col,std::optional<PropertyValue> value) {
                        if (col) throw std::runtime_error("changed column");
                        if (!value) return;
                        bool match = false;
                        if (text && value->getType()==PropertyType::STRING) match=std::get<std::string>(value->getVariant())=="CN";
                        if (!text && value->getType()==PropertyType::DOUBLE) match=std::get<double>(value->getVariant())>=900.;
                        if (!text && value->getType()==PropertyType::INTEGER) match=std::get<int64_t>(value->getVariant())>=900;
                        set(row,match);
                    };
                    PropertyColumnBatchReadSinks runs;
                    runs.scalars = [&](auto rows,size_t col,PropertyType type,auto words) {
                        if (col || rows.size()!=words.size()) throw std::runtime_error("changed run");
                        for(size_t i=0;i<rows.size();++i) set(rows[i], !text &&
                            ((type==PropertyType::DOUBLE && std::bit_cast<double>(words[i])>=900.) ||
                             (type==PropertyType::INTEGER && std::bit_cast<int64_t>(words[i])>=900)));
                    };
                    runs.texts = [&](auto rows,size_t col,auto words) {
                        if(col || rows.size()!=words.size()) throw std::runtime_error("changed text run");
                        for(size_t i=0;i<rows.size();++i)set(rows[i],text && words[i]=="CN");
                    };
                    const auto start = Clock::now();
                    read(ids,owning,runs,[&](size_t bytes){++calls;sourceCredit.set(sourceCredit.bytes()+bytes);});
                    sourceMs += std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                    for(size_t i=0;i<ids.size();++i) matches+=matched[i];
                    return true;
                },[&](size_t bytes){batchCredit.set(bytes);});
            const double totalMs=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
            if(!completed.value_or(false))throw std::runtime_error("incomplete certified source");
            if(iteration>=0)std::cout<<"{\"case\":\""<<(text?"label_scan_filter":"all_nodes_property_filter")
                <<"\",\"iteration\":"<<iteration<<",\"examined\":"<<examined<<",\"selected\":"<<selected
                <<",\"values\":"<<values<<",\"count\":"<<matches<<",\"admission_calls\":"<<calls
                <<",\"batches\":"<<batches<<",\"source_and_callback_ms\":"<<sourceMs
                <<",\"provider_and_consumer_ms\":"<<totalMs<<"}\n"<<std::flush;
        }
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
