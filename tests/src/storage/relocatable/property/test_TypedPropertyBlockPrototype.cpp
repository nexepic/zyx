#include <gtest/gtest.h>
#include <limits>
#include "benchmarks/graphdb-comparison/property-block-prototype/TypedPropertyBlock.hpp"
using zyx::benchmark::TypedPropertyBlock;
using graph::PropertyValue;
using graph::storage::PropertyCollectionRoot;
using graph::storage::LittleEndianCodec;
namespace {
std::vector<PropertyCollectionRoot> owners() {
 return {{7U, 1U, {{1U, PropertyValue(int64_t(-91))}, {2U, PropertyValue()}, {3U, PropertyValue("large")}, {5U, PropertyValue(true)}, {9U, PropertyValue(1.25)}}},
         {23U, 1U, {{1U, PropertyValue((std::numeric_limits<int64_t>::min)())}, {3U, PropertyValue(int64_t(8))}, {5U, PropertyValue(false)}, {9U, PropertyValue(-4.5)}}},
         {99U, 1U, {{1U, PropertyValue()}, {4U, PropertyValue(std::vector<PropertyValue>{PropertyValue(2), PropertyValue("nested")})}}}};
}
template<class F> void mutateColumn(TypedPropertyBlock::Image &image, size_t index, F &&mutate) {
 auto copy = *image.columns[index]; mutate(copy);
 image.columns[index] = std::make_shared<const std::vector<std::byte>>(std::move(copy));
}
void refreshMetadata(TypedPropertyBlock::Image &image) {
 LittleEndianCodec::writeU32(image.metadata, 24U, 0U);
 LittleEndianCodec::writeU32(image.metadata, 24U, graph::utils::calculateCrc(image.metadata.data(), image.metadata.size()));
}
}
TEST(TypedPropertyBlockPrototypeTest, RoundTripsTypedMixedAndNestedValuesWithIndependentPresenceAndNull) {
 const auto input = owners(); const auto image = TypedPropertyBlock::encode(11U, input);
 const auto keys = TypedPropertyBlock::tokens(image);
 EXPECT_EQ(keys, (std::vector<uint32_t>{1U,2U,3U,4U,5U,9U}));
 EXPECT_EQ(TypedPropertyBlock::decode(image, 11U, 1U, keys), input);
 const std::vector<uint32_t> selected{1U,2U,8U};
 auto projected = TypedPropertyBlock::decode(image, 11U, 1U, selected);
 ASSERT_EQ(projected.size(), 3U);
 EXPECT_EQ(projected[0].entries, (std::vector<graph::storage::PropertyCollectionRootEntry>{{1U, PropertyValue(int64_t(-91))},{2U,PropertyValue()}}));
 EXPECT_EQ(projected[1].entries.size(), 1U);
 EXPECT_EQ(projected[2].entries, (std::vector<graph::storage::PropertyCollectionRootEntry>{{1U,PropertyValue()}}));
}
TEST(TypedPropertyBlockPrototypeTest, RejectsCorruptSelectedPayloadButDoesNotReadUnselectedColumns) {
 auto image = TypedPropertyBlock::encode(11U, owners()); mutateColumn(image,2,[](auto &bytes){bytes.back() ^= std::byte{1};});
 const std::vector<uint32_t> narrow{1U}, corrupt{3U};
 EXPECT_NO_THROW(static_cast<void>(TypedPropertyBlock::decode(image, 11U, 1U, narrow)));
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(image, 11U, 1U, corrupt)), std::runtime_error);
 image.metadata.back() ^= std::byte{1};
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(image, 11U, 1U, narrow)), std::runtime_error);
}
TEST(TypedPropertyBlockPrototypeTest, RejectsChecksumValidBadIdentityMasksDirectoryAndKind) {
 const auto valid = TypedPropertyBlock::encode(11U, owners()); const std::vector<uint32_t> keys{1U};
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(valid, 12U, 1U, keys)), std::runtime_error);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(valid, 11U, 2U, keys)), std::runtime_error);
 for (size_t offset : {size_t{4}, size_t{28}, size_t{40}, size_t{60}, size_t{64}, size_t{72}, size_t{80}, size_t{92}}) {
  auto bad = valid;
  if (offset == 40U) LittleEndianCodec::writeU64(bad.metadata, offset, 7U); // duplicate owner
  else if (offset == 64U) LittleEndianCodec::writeU64(bad.metadata, offset, uint64_t{1} << 63U); // presence outside rows
  else if (offset == 72U) LittleEndianCodec::writeU64(bad.metadata, offset, 8U); // null without presence
  else LittleEndianCodec::writeU32(bad.metadata, offset, UINT32_MAX);
  refreshMetadata(bad);
  EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(bad, 11U, 1U, keys)), std::runtime_error) << offset;
 }
 auto bad = valid; mutateColumn(bad,0,[](auto &bytes){bytes.pop_back();});
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(bad, 11U, 1U, keys)), std::runtime_error);
 const std::vector<uint32_t> duplicate{1U,1U};
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(valid, 11U, 1U, duplicate)), std::runtime_error);
}
TEST(TypedPropertyBlockPrototypeTest, ReplacementPreservesOwnerSlotsAndImmutableOldImage) {
 const auto input = owners(); const auto old = TypedPropertyBlock::encode(11U, input);
 auto next = TypedPropertyBlock::replace(old, 11U, 1U, 23U, {{2U,PropertyValue()}, {3U,PropertyValue("changed")}});
 const auto keys = TypedPropertyBlock::tokens(next); auto expected = input;
 expected[1].entries = {{2U,PropertyValue()}, {3U,PropertyValue("changed")}};
 EXPECT_EQ(TypedPropertyBlock::decode(next, 11U, 1U, keys), expected);
 const auto oldKeys = TypedPropertyBlock::tokens(old);
 EXPECT_EQ(TypedPropertyBlock::decode(old, 11U, 1U, oldKeys), input);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::replace(old, 11U, 1U, 23U, {{2U,PropertyValue(1)},{2U,PropertyValue(2)}})), std::runtime_error);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::replace(old, 11U, 1U, 88U, {})), std::runtime_error);
 EXPECT_EQ(TypedPropertyBlock::decode(old, 11U, 1U, oldKeys), input);
}
TEST(TypedPropertyBlockPrototypeTest, Supports64SlotsAndEmptyCollectionsRejectsUnstableMappings) {
 std::vector<PropertyCollectionRoot> input;
 for (uint64_t row=1; row<=64; ++row) input.push_back({row*7, 1U, {{3U,PropertyValue(row)}}});
 auto image = TypedPropertyBlock::encode(1U,input); const std::vector<uint32_t> keys{3U};
 EXPECT_EQ(TypedPropertyBlock::decode(image,1U,1U,keys),input);
 input.push_back({1000U,1U,{}}); EXPECT_THROW(static_cast<void>(TypedPropertyBlock::encode(1U,input)),std::runtime_error);
 input = {{7U,1U,{}},{23U,1U,{}}}; image=TypedPropertyBlock::encode(1U,input);
 EXPECT_EQ(TypedPropertyBlock::decode(image,1U,1U,{}),input);
 input[1].owner_entity_id=7U; EXPECT_THROW(static_cast<void>(TypedPropertyBlock::encode(1U,input)),std::runtime_error);
 input=owners(); input[1].owner_entity_type=2U; EXPECT_THROW(static_cast<void>(TypedPropertyBlock::encode(1U,input)),std::runtime_error);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::encode(0U,owners())),std::runtime_error);
}
TEST(TypedPropertyBlockPrototypeTest, RejectsChecksumValidMalformedOffsetsBooleanAndNullPayloads) {
 const auto valid=TypedPropertyBlock::encode(11U,owners()); const std::vector<uint32_t> keys{1U,3U,5U};
 for(size_t column: {size_t{0},size_t{2},size_t{4}}) {
  auto bad=valid;
  if(column==0) mutateColumn(bad,column,[](auto &bytes){bytes[16]=std::byte{1};}); // NULL integer slot must be zero
  if(column==2) mutateColumn(bad,column,[](auto &bytes){LittleEndianCodec::writeU32(bytes,0U,0U);}); // offsets cannot target their own directory
  if(column==4) mutateColumn(bad,column,[](auto &bytes){bytes[0]=std::byte{2};}); // noncanonical true
  const size_t descriptor=TypedPropertyBlock::HEADER+3*8+column*TypedPropertyBlock::DESCRIPTOR;
  LittleEndianCodec::writeU32(bad.metadata,descriptor+32,graph::utils::calculateCrc(bad.columns[column]->data(),bad.columns[column]->size()));
  refreshMetadata(bad);
  EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(bad,11U,1U,keys)),std::runtime_error);
 }
}
TEST(TypedPropertyBlockPrototypeTest, PreservesSignedZeroAndNonfiniteDoubleBits) {
 const uint64_t bits=0x7ff8000000000042ULL;
 const std::vector<PropertyCollectionRoot> input{{7U,1U,{{1U,PropertyValue(-0.0)}}},{23U,1U,{{1U,PropertyValue(std::bit_cast<double>(bits))}}}};
 const auto image=TypedPropertyBlock::encode(11U,input); const std::vector<uint32_t> keys{1U};
 const auto result=TypedPropertyBlock::decode(image,11U,1U,keys);
 ASSERT_EQ(result.size(),2U);
 EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(result[0].entries[0].value.getVariant())),std::bit_cast<uint64_t>(-0.0));
 EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(result[1].entries[0].value.getVariant())),bits);
}
TEST(TypedPropertyBlockPrototypeTest, SingleFieldCowSharesSiblingColumnsAndNoOpWritesNothing) {
 const auto input=owners(); const auto old=TypedPropertyBlock::encode(11U,input);
 auto replacement=input[0].entries; replacement[0].value=PropertyValue(int64_t(55));
 const auto next=TypedPropertyBlock::replace(old,11U,1U,7U,replacement);
 ASSERT_EQ(next.columns.size(),old.columns.size()); EXPECT_NE(next.columns[0],old.columns[0]);
 for(size_t i=1;i<old.columns.size();++i) EXPECT_EQ(next.columns[i],old.columns[i]);
 EXPECT_EQ(TypedPropertyBlock::rewrittenBytes(old,next),next.metadata.size()+next.columns[0]->size());
 const auto noOp=TypedPropertyBlock::replace(old,11U,1U,7U,input[0].entries);
 EXPECT_EQ(TypedPropertyBlock::rewrittenBytes(old,noOp),0U);
 EXPECT_EQ(noOp.columns,old.columns);
}
TEST(TypedPropertyBlockPrototypeTest, CowDeletesLastPresenceAndAddsColumnsWithoutMovingOwners) {
 const auto old=TypedPropertyBlock::encode(11U,owners());
 const auto next=TypedPropertyBlock::replace(old,11U,1U,7U,{{1U,PropertyValue(55)},{7U,PropertyValue("new")}});
 const auto keys=TypedPropertyBlock::tokens(next);
 EXPECT_EQ(keys,(std::vector<uint32_t>{1U,3U,4U,5U,7U,9U})); // token 2 was present only in owner 7
 auto expected=owners();expected[0].entries={{1U,PropertyValue(55)},{7U,PropertyValue("new")}};
 EXPECT_EQ(TypedPropertyBlock::decode(next,11U,1U,keys),expected);
 const auto oldKeys=TypedPropertyBlock::tokens(old);
 EXPECT_EQ(TypedPropertyBlock::decode(old,11U,1U,oldKeys),owners());
}
TEST(TypedPropertyBlockPrototypeTest, CowEqualityUsesWireIdentityForSignedZeroAndNan) {
 const uint64_t nan=0x7ff8000000000042ULL;
 const std::vector<PropertyCollectionRoot> input{{7U,1U,{{1U,PropertyValue(-0.0)},{2U,PropertyValue(std::bit_cast<double>(nan))}}}};
 const auto old=TypedPropertyBlock::encode(11U,input);
 const auto noOp=TypedPropertyBlock::replace(old,11U,1U,7U,input[0].entries);
 EXPECT_EQ(TypedPropertyBlock::rewrittenBytes(old,noOp),0U);
 auto changed=input[0].entries;changed[0].value=PropertyValue(0.0);
 const auto next=TypedPropertyBlock::replace(old,11U,1U,7U,changed);
 EXPECT_NE(old.columns[0],next.columns[0]);EXPECT_EQ(old.columns[1],next.columns[1]);
 const std::vector<uint32_t> keys{1U};const auto decoded=TypedPropertyBlock::decode(next,11U,1U,keys);
 EXPECT_EQ(std::bit_cast<uint64_t>(std::get<double>(decoded[0].entries[0].value.getVariant())),uint64_t{0});
}
TEST(TypedPropertyBlockPrototypeTest, VerifiedReplacementRejectsForgedInputsAndWrongIdentity) {
 auto raw=TypedPropertyBlock::encode(11U,owners());
 mutateColumn(raw,2,[](auto &bytes){bytes.back() ^= std::byte{1};});
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::verify(raw,11U,1U)),std::runtime_error);
 const auto good=TypedPropertyBlock::encode(11U,owners());
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::verify(good,12U,1U)),std::runtime_error);
 const auto proof=TypedPropertyBlock::verify(good,11U,1U);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::replaceVerified(proof,12U,1U,7U,{})),std::runtime_error);
 auto expected=owners();expected[0].entries[0].value=PropertyValue(87);
 const auto next=TypedPropertyBlock::replaceVerified(proof,11U,1U,7U,expected[0].entries);
 const auto keys=TypedPropertyBlock::tokens(next.image());
 EXPECT_EQ(TypedPropertyBlock::decode(next.image(),11U,1U,keys),expected);
 EXPECT_EQ(TypedPropertyBlock::decode(proof.image(),11U,1U,keys),owners());
 for(size_t i=1;i<proof.image().columns.size();++i) EXPECT_EQ(proof.image().columns[i],next.image().columns[i]);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::replaceVerified(proof,11U,1U,7U,{{1U,PropertyValue(1)},{1U,PropertyValue(2)}})),std::runtime_error);
 EXPECT_EQ(TypedPropertyBlock::decode(proof.image(),11U,1U,keys),owners());
}
TEST(TypedPropertyBlockPrototypeTest, ProofFreezesExternalMutableAliasesAndRawMetadata) {
 auto raw=TypedPropertyBlock::encode(11U,owners());
 auto alias=std::make_shared<std::vector<std::byte>>(*raw.columns[2]);raw.columns[2]=alias;
 const auto proof=TypedPropertyBlock::verify(raw,11U,1U);
 EXPECT_NE(raw.columns[2],proof.image().columns[2]);
 alias->back() ^= std::byte{1};raw.metadata[0] ^= std::byte{1};
 const auto keys=TypedPropertyBlock::tokens(proof.image());
 EXPECT_EQ(TypedPropertyBlock::decode(proof.image(),11U,1U,keys),owners());
 auto expected=owners();expected[1].entries={{3U,PropertyValue("changed")}};
 const auto next=TypedPropertyBlock::replaceVerified(proof,11U,1U,23U,expected[1].entries);
 const auto nextKeys=TypedPropertyBlock::tokens(next.image());
 EXPECT_EQ(TypedPropertyBlock::decode(next.image(),11U,1U,nextKeys),expected);
}
TEST(TypedPropertyBlockPrototypeTest, EmptyVersionIsExclusiveAndMetadataShapeBoundsPrecedeChecksumCopy) {
 const auto empty=TypedPropertyBlock::encodeEmpty(11,1);const std::vector<uint32_t> tokens{1};
 EXPECT_TRUE(TypedPropertyBlock::decode(empty,11,1,tokens).empty());
 for(int mode=0;mode<4;++mode) {auto bad=empty;
  if(mode==0)LittleEndianCodec::writeU16(bad.metadata,4,1);
  if(mode==1)LittleEndianCodec::writeU16(bad.metadata,6,65);
  if(mode==2)LittleEndianCodec::writeU32(bad.metadata,8,4097);
  if(mode==3)bad.metadata.resize(1024*1024);
  refreshMetadata(bad);
  EXPECT_THROW(static_cast<void>(TypedPropertyBlock::verify(bad,11,1)),std::runtime_error);
 }
 auto nonempty=TypedPropertyBlock::encode(11,owners());LittleEndianCodec::writeU16(nonempty.metadata,4,2);refreshMetadata(nonempty);
 EXPECT_THROW(static_cast<void>(TypedPropertyBlock::decode(nonempty,11,1,tokens)),std::runtime_error);
}
