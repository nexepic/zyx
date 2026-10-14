// Laboratory carriers only. These values have no authoritative graph route or
// automatic retirement and therefore cannot be admitted as production layouts.
#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include <map>
#include <tuple>
namespace {
struct CandidateEntry { uint64_t node, edge, neighbor, type; bool operator==(const CandidateEntry &) const = default; };
void appendWord(std::vector<std::byte> &out, uint64_t value) {
	for (unsigned i=0;i<8;++i) out.push_back(static_cast<std::byte>((value>>(8*i))&255U));
}
uint64_t word(std::span<const std::byte> bytes, size_t offset) {
	if(offset>bytes.size() || bytes.size()-offset<8) throw std::invalid_argument("short candidate word");
	uint64_t value=0;for(unsigned i=0;i<8;++i)value|=uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(8*i);return value;
}
std::vector<std::byte> encode(std::span<const CandidateEntry> entries) {
	std::vector<std::byte> bytes;
	for(const auto &e:entries){appendWord(bytes,e.node);appendWord(bytes,e.edge);appendWord(bytes,e.neighbor);appendWord(bytes,e.type);}return bytes;
}
CandidateEntry decode(std::span<const std::byte> bytes) {
	if(bytes.size()!=32U)throw std::invalid_argument("wrong candidate width");
	return {word(bytes,0),word(bytes,8),word(bytes,16),word(bytes,24)};
}
void verify(const CandidateEntry &e,const std::map<uint64_t,CandidateEntry> &masters) {
	const auto it=masters.find(e.edge);
	if(it==masters.end()||it->second!=e)throw std::invalid_argument("candidate disagrees with master");
}
LargeValueWriteOptions carrierOptions() {
	LargeValueWriteOptions o;o.first_data_slot=PhysicalSlotId(40);o.last_data_slot=PhysicalSlotId(80);
	o.first_manifest_slot=PhysicalSlotId(81);o.last_manifest_slot=PhysicalSlotId(90);
	o.kind=LargeValueKind::LVK_BYTES;o.media_type="application/test-adjacency-candidate";return o;
}
}
TEST_F(RelocatableMetadataStoreTest, AdjacencyCandidateCarriersPreserveLowDegreeHubParallelAndLoopEntries) {
	auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));LargeValueStore store(io,*metadata);
	std::vector<CandidateEntry> entries;std::map<uint64_t,CandidateEntry> masters;
	for(uint64_t i=1;i<=1100;++i){CandidateEntry e{i<100?i:100,i,i%7,i%3};entries.push_back(e);masters.emplace(i,e);}
	// Shared packed locator candidate: a global (node,edge)->ordinal map routes
	// into one shared topology value; no per-node exclusive data page.
	std::map<std::pair<uint64_t,uint64_t>,size_t> locators;
	for(size_t i=0;i<entries.size();++i)locators.emplace(std::pair{entries[i].node,entries[i].edge},i);
	const auto bytes=encode(entries);const auto packed=store.write(bytes,carrierOptions(),LogSequenceNumber(2));
	// Shared segmented candidate: block descriptors cover up to 64 entries;
	// boundaries deliberately do not allocate one low-degree-node tail.
	auto segmentedOptions=carrierOptions();
	for(size_t end=64U*32U;end<bytes.size();end+=64U*32U)segmentedOptions.compression.logical_chunk_ends.push_back(end);
	// Distinct candidate trailer prevents content deduplication from silently
	// retaining the first layout, which would invalidate this comparison.
	auto segmentedBytes=bytes;appendWord(segmentedBytes,64U);
	const auto segmented=store.write(segmentedBytes,segmentedOptions,LogSequenceNumber(3));
	auto pin=metadata->openSnapshot();
	for(const auto &[key,ordinal]:locators){
		const auto covered=decode(store.readRange(pin,packed.reference,ordinal*32U,32U));verify(covered,masters);EXPECT_EQ(covered,entries[ordinal]);
	}
	size_t visited=0;
	for(size_t start=0;start<entries.size();start+=64){
		const auto count=std::min<size_t>(64,entries.size()-start);
		const auto block=store.readRange(pin,segmented.reference,start*32U,count*32U);
		for(size_t i=0;i<count;++i){auto e=decode(std::span<const std::byte>(block).subspan(i*32U,32U));verify(e,masters);EXPECT_EQ(e,entries[start+i]);++visited;}
	}
	EXPECT_EQ(visited,entries.size());
	EXPECT_GT(segmented.manifest.chunks.size(),packed.manifest.chunks.size());
	RecordProperty("packed_chunks",std::to_string(packed.manifest.chunks.size()));
	RecordProperty("segmented_chunks",std::to_string(segmented.manifest.chunks.size()));
	// Captured old carriers remain readable after another COW publication.
	entries.back().type=99;const auto changed=store.write(encode(entries),carrierOptions(),LogSequenceNumber(4));
	EXPECT_EQ(store.read(pin,packed.reference),bytes);auto fresh=metadata->openSnapshot();
	EXPECT_EQ(store.read(fresh,changed.reference),encode(entries));
	pin.release();fresh.release();metadata.reset();metadata=RelocatableMetadataStore::open(io);LargeValueStore reopened(io,*metadata);
	auto restored=metadata->openSnapshot();EXPECT_EQ(reopened.read(restored,segmented.reference),segmentedBytes);
}
TEST(AdjacencyCandidateFaultTest, ValidBytesCannotHideDisagreementBehindTypeFiltering) {
	const CandidateEntry original{1,7,2,3};const std::map<uint64_t,CandidateEntry> masters{{7,original}};
	for(auto bad:std::vector<CandidateEntry>{{9,7,2,3},{1,7,9,3},{1,7,2,9},{1,8,2,3}}){
		const auto decoded=decode(encode(std::span<const CandidateEntry>(&bad,1)));
		EXPECT_THROW(verify(decoded,masters),std::invalid_argument);
	}
	EXPECT_THROW(decode(std::vector<std::byte>(31)),std::invalid_argument);
	EXPECT_NO_THROW(verify(original,masters));
}

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
TEST_F(RelocatableMetadataStoreTest, CoveredCandidateNativeU64LeafRoutesToAuthenticatedSharedTopologyCarrier) {
	auto metadata=RelocatableMetadataStore::initializeNew(io,superblock(1,1));LargeValueStore values(io,*metadata);
	std::vector<CandidateEntry> entries;std::map<uint64_t,CandidateEntry> masters;
	for(uint64_t id=1;id<=32;++id){CandidateEntry e{id<8?id:8,id,id%5,id%3};entries.push_back(e);masters.emplace(id,e);}
	const auto carrier=values.write(encode(entries),carrierOptions(),LogSequenceNumber(2));
	// A separate laboratory namespace. Never reinterpret the reserved EMPTY tree.
	constexpr uint64_t candidateIndex=0x434F564552ULL;
	SlottedPagePool pool(io,*metadata);BTreePageStore trees(pool,*metadata);
	BTreePageHeader header;header.index_id=candidateIndex;header.page_id=PageId(1001);header.generation=Generation(1);header.leaf_value_layout=BTreeLeafValueLayout::BLVL_U64;
	PageOwnerHeader owner{header.page_id,candidateIndex,PageOwnerKind::POK_INDEX,0,RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,Generation(1),LogSequenceNumber(3)};
	auto page=BTreePage::create(owner,header);
	for(size_t i=0;i<entries.size();++i)page.insert({AdjacencyKeyCodec::encode({GlobalAdjacencyDirection::GAD_OUT,entries[i].node,entries[i].edge}),i});
	SlottedPagePoolAllocationRequest request;request.segment_id=SegmentId(101);request.pool_kind=SegmentKind::SGK_INDEX_PAGE_POOL;request.pool_owner_id=candidateIndex;request.generation=Generation(1);request.page_id=header.page_id;request.page_owner_id=candidateIndex;request.page_owner_kind=PageOwnerKind::POK_INDEX;
	ASSERT_TRUE(trees.allocate(request,page,PhysicalSlotId(101),PhysicalSlotId(101),LogSequenceNumber(3)).has_value());
	auto snapshot=metadata->openSnapshot();const auto restored=trees.read(snapshot,header.page_id);
	for(const auto &entry:restored.entries()){
		const auto covered=decode(values.readRange(snapshot,carrier.reference,entry.value_or_child*32U,32U));
		verify(covered,masters);const auto key=AdjacencyKeyCodec::decode(entry.key);
		EXPECT_EQ(key.node_id,covered.node);EXPECT_EQ(key.edge_id,covered.edge);
	}
}
