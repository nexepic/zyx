#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
namespace graph::query::execution::detail {
// Query-private row bytes: 8-byte magic, u64 ordinal, u32 binding count,
// u32 key count, then bindings (u32 name bytes, name, u8 kind, value), keys.
// Kind 1 is a canonical primitive/dynamic PropertyValue; kind 2 is a logical
// EntityRef (five u64 fields). No pointer, physical page or cache is persisted.
// Graph property caches may be reread through the original pinned snapshot.
class QuerySpillRowCodec final {
public:
	struct Record { LogicalPlanReadRow row;std::vector<PropertyValue> keys;uint64_t ordinal=0; };
	static constexpr uint64_t MAGIC=UINT64_C(0x3157525358595A); // ZYXSRW1
	static constexpr size_t HEADER_BYTES=24U;
	static size_t encodedSize(const Record &record) {
		if(!record.row.path_cache.empty() || record.row.values.size()>4096U || record.keys.size()>4096U)
			throw std::runtime_error("query spill does not support paths or over-wide rows");
		size_t total=HEADER_BYTES;
		for(const auto &[name,value]:record.row.values){
			if(name.empty() || name.size()>UINT32_MAX)throw std::invalid_argument("query spill binding name is invalid");
			total=add(total,add(5U,name.size()));
			if(const auto *primitive=std::get_if<PropertyValue>(&value))total=add(total,storage::CanonicalPropertyValueCodec::encodedSize(*primitive));
			else if(const auto *ref=std::get_if<storage::EntityRef>(&value)){validate(*ref);total=add(total,40U);}
			else throw std::runtime_error("query spill does not support complete entity owners");
		}
		for(const auto &key:record.keys)total=add(total,storage::CanonicalPropertyValueCodec::encodedSize(key));
		return total;
	}
	static std::vector<std::byte> encode(const Record &record,size_t maximumBytes,std::pmr::memory_resource *resource=std::pmr::get_default_resource()) {
		const size_t size=encodedSize(record);if(size>maximumBytes)throw std::runtime_error("query spill row exceeded its encoded byte bound");
		std::vector<std::byte> out;out.reserve(size);word(out,MAGIC,8);word(out,record.ordinal,8);word(out,record.row.values.size(),4);word(out,record.keys.size(),4);
		// Canonical binding order is independent of unordered-map allocation/order.
		using Binding=decltype(record.row.values)::value_type;
		std::pmr::vector<const Binding *> names(resource);names.reserve(record.row.values.size());for(const auto &binding:record.row.values)names.push_back(&binding);std::ranges::sort(names,[](const auto *a,const auto *b){return a->first<b->first;});
		for(const auto *binding:names){const auto &name=binding->first;word(out,name.size(),4);for(const char byte:name)out.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			const auto &value=binding->second;
			if(const auto *primitive=std::get_if<PropertyValue>(&value)){word(out,1,1);storage::CanonicalPropertyValueCodec::append(out,*primitive);}
			else {const auto &ref=std::get<storage::EntityRef>(value);word(out,2,1);word(out,ref.entity_id,8);word(out,ref.is_edge?1:0,8);word(out,ref.source_node_id,8);word(out,ref.target_node_id,8);word(out,ref.type_token,8);}
		}
		for(const auto &key:record.keys)storage::CanonicalPropertyValueCodec::append(out,key);
		if(out.size()!=size)throw std::logic_error("query spill row size changed while encoding");return out;
	}
	static Record decode(std::span<const std::byte> bytes,size_t maximumBytes) {
		if(bytes.size()<HEADER_BYTES || bytes.size()>maximumBytes)throw std::invalid_argument("query spill row dimensions are invalid");
		size_t offset=0;if(read(bytes,offset,8)!=MAGIC)throw std::invalid_argument("query spill row magic/version is invalid");
		Record record;record.ordinal=read(bytes,offset,8);const auto bindings=read(bytes,offset,4),keys=read(bytes,offset,4);
		// Reject hostile counts before reserve/decoding, even with valid outer CRC.
		if(bindings>4096U || keys>4096U || bindings>(bytes.size()-offset)/12U || keys>(bytes.size()-offset)/storage::CanonicalPropertyValueCodec::HEADER_BYTES)
			throw std::invalid_argument("query spill row counts exceed encoded bounds");
		std::string previous;
		for(size_t i=0;i<bindings;++i){const auto count=read(bytes,offset,4);if(count==0 || count>bytes.size()-offset)throw std::invalid_argument("query spill binding is truncated");
			std::string name(reinterpret_cast<const char *>(bytes.data()+offset),static_cast<size_t>(count));offset+=count;
			if(i!=0 && name<=previous)throw std::invalid_argument("query spill bindings are duplicate or unordered");previous=name;
			const auto kind=read(bytes,offset,1);
			if(kind==1)record.row.values.emplace(std::move(name),storage::CanonicalPropertyValueCodec::decodeNext(bytes,offset));
			else if(kind==2){storage::EntityRef ref;ref.entity_id=read(bytes,offset,8);const auto edge=read(bytes,offset,8);if(edge>1)throw std::invalid_argument("query spill entity tag is invalid");ref.is_edge=edge==1;ref.source_node_id=read(bytes,offset,8);ref.target_node_id=read(bytes,offset,8);ref.type_token=read(bytes,offset,8);validate(ref);record.row.values.emplace(std::move(name),ref);}
			else throw std::invalid_argument("query spill binding kind is invalid");
		}
		record.keys.reserve(static_cast<size_t>(keys));for(size_t i=0;i<keys;++i)record.keys.push_back(storage::CanonicalPropertyValueCodec::decodeNext(bytes,offset));
		if(offset!=bytes.size())throw std::invalid_argument("query spill row has trailing bytes");return record;
	}
private:
	static size_t add(size_t a,size_t b){if(b>SIZE_MAX-a)throw std::overflow_error("query spill row size overflowed");return a+b;}
	static void word(std::vector<std::byte> &out,uint64_t value,size_t width){for(size_t i=0;i<width;++i)out.push_back(static_cast<std::byte>((value>>(8*i))&255U));}
	static uint64_t read(std::span<const std::byte> bytes,size_t &offset,size_t width){if(offset>bytes.size() || width>bytes.size()-offset)throw std::invalid_argument("query spill scalar is truncated");uint64_t value=0;for(size_t i=0;i<width;++i)value|=uint64_t(std::to_integer<unsigned>(bytes[offset+i]))<<(8*i);offset+=width;return value;}
	static void validate(const storage::EntityRef &ref){if(ref.entity_id==0 || (ref.is_edge && (ref.source_node_id==0 || ref.target_node_id==0 || ref.type_token==0)) || (!ref.is_edge && (ref.source_node_id!=0 || ref.target_node_id!=0 || ref.type_token!=0)))throw std::invalid_argument("query spill EntityRef is malformed");}
};
}
