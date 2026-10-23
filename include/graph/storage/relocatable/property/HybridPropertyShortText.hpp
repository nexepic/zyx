#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {
// A bounded byte representation, independent of host endianness. The enclosing
// authenticated column owns persistence, namespace, masks and snapshot identity.
class HybridPropertyShortText final {
public:
 static constexpr size_t MAX_BYTES=7;
 [[nodiscard]] static std::optional<uint64_t> encode(std::string_view text) {
  if(text.size()>MAX_BYTES)return std::nullopt;
  uint64_t word=uint64_t(text.size())<<56U;
  for(size_t i=0;i<text.size();++i)word|=uint64_t(static_cast<unsigned char>(text[i]))<<(i*8U);
  return word;
 }
 static void validate(uint64_t word) {
  const auto length=word>>56U;
  if(length>MAX_BYTES)throw FormatException(FormatError::FERR_INVALID_HEADER,"short-text length exceeds word capacity");
  const uint64_t mask=length==MAX_BYTES?UINT64_C(0x00ffffffffffffff):(UINT64_C(1)<<(length*8U))-1U;
  if((word&UINT64_C(0x00ffffffffffffff)&~mask)!=0)
   throw FormatException(FormatError::FERR_INVALID_HEADER,"short-text unused payload is nonzero");
 }
 [[nodiscard]] static std::string_view copy(uint64_t word,std::array<char,MAX_BYTES> &buffer) {
  validate(word);const size_t length=word>>56U;
  for(size_t i=0;i<length;++i)buffer[i]=static_cast<char>((word>>(i*8U))&0xffU);
  return {buffer.data(),length};
 }
 [[nodiscard]] static std::string decode(uint64_t word) {
  std::array<char,MAX_BYTES> buffer{};return std::string(copy(word,buffer));
 }
};
}
