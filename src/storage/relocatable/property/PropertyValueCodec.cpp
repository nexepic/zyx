/**
 * @file PropertyValueCodec.cpp
 * @brief Canonical, bounded PropertyValue wire codec for native storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/query/execution/StandardContainerHeapBytes.hpp"
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/debug/PerfTrace.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace graph::storage {

	namespace {

		constexpr uint32_t MAX_NESTING_DEPTH = 64U;

		enum class PropertyValueWireKind : uint8_t {
			PVW_NULL = 1, PVW_BOOLEAN = 2, PVW_INTEGER = 3, PVW_DOUBLE = 4, PVW_STRING = 5,
			PVW_LIST = 6, PVW_MAP = 7, PVW_DATE = 8, PVW_DATETIME = 9, PVW_DURATION = 10, PVW_BYTES = 11,
		};

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool bytewiseLess(std::string_view lhs, std::string_view rhs) {
			return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), [](char left, char right) {
				return static_cast<unsigned char>(left) < static_cast<unsigned char>(right);
			});
		}

		template<class Bytes> void appendU8(Bytes &bytes, uint8_t value) { bytes.push_back(static_cast<std::byte>(value)); }
		template<class Bytes> void appendU32(Bytes &bytes, uint32_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU32(bytes, offset, value);
		}
		template<class Bytes> void appendU64(Bytes &bytes, uint64_t value) {
			const size_t offset = bytes.size();
			bytes.resize(offset + sizeof(value));
			LittleEndianCodec::writeU64(bytes, offset, value);
		}
		template<class Bytes> void appendBytes(Bytes &bytes, std::string_view value) {
			bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(value.data()),
						 reinterpret_cast<const std::byte *>(value.data() + value.size()));
		}
		template<class Bytes> void appendBytes(Bytes &bytes, std::span<const std::byte> value) {
			bytes.insert(bytes.end(), value.begin(), value.end());
		}
		void requireU32Size(size_t value, const char *message) {
			if (value > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
		}

		struct ByteReader {
			using Key = std::string_view;
			std::span<const std::byte> bytes;
			size_t offset = 0U;
			[[nodiscard]] size_t remaining() const { return bytes.size() - offset; }
			[[nodiscard]] uint8_t readU8() {
				if (remaining() < 1U) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value is truncated");
				return static_cast<uint8_t>(bytes[offset++]);
			}
			[[nodiscard]] uint32_t readU32() {
				if (remaining() < sizeof(uint32_t)) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value is truncated");
				const uint32_t value = LittleEndianCodec::readU32(bytes, offset);
				offset += sizeof(value);
				return value;
			}
			[[nodiscard]] uint64_t readU64() {
				if (remaining() < sizeof(uint64_t)) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value is truncated");
				const uint64_t value = LittleEndianCodec::readU64(bytes, offset);
				offset += sizeof(value);
				return value;
			}
            [[nodiscard]] std::string_view readStringView(uint32_t count) {
                if (count > remaining()) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property string is truncated");
                const char *data = reinterpret_cast<const char *>(bytes.data() + offset);
                offset += count; return {data, count};
            }
            [[nodiscard]] std::string readString(uint32_t count) { return std::string(readStringView(count)); }
			[[nodiscard]] std::span<const std::byte> readBytes(uint32_t count) {
				if (count > remaining()) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property payload is truncated");
				const auto result = bytes.subspan(offset, count);
				offset += count;
				return result;
			}
            [[nodiscard]] ByteReader subReader(uint32_t count) { return {readBytes(count)}; }
            void skip(uint32_t count) { static_cast<void>(readBytes(count)); }
            [[nodiscard]] Key readKey(uint32_t count) { return readStringView(count); }
            [[nodiscard]] bool keyLess(Key left, Key right) const { return bytewiseLess(left, right); }
		};

        // All offsets remain within the caller's immutable value range. Child
        // readers carry limits, rather than allocating a payload-sized buffer.
        struct RangeInspectorReader {
            struct Key { size_t offset = 0, count = 0; };
            const CanonicalPropertyValueCodec::RangeReader &read;
            const std::function<void()> &checkpoint;
            size_t offset = 0, end = 0;
            [[nodiscard]] size_t remaining() const { return end - offset; }
            void require(size_t count) const {
                if (count > remaining()) fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property range is truncated");
            }
            template<size_t Count> [[nodiscard]] std::array<std::byte, Count> word() {
                require(Count);
                std::array<std::byte, Count> bytes;
                if (checkpoint) checkpoint();
                read(offset, bytes); offset += Count;
                return bytes;
            }
            [[nodiscard]] uint8_t readU8() { return std::to_integer<uint8_t>(word<1>()[0]); }
            [[nodiscard]] uint32_t readU32() { return LittleEndianCodec::readU32(word<4>(), 0); }
            [[nodiscard]] uint64_t readU64() { return LittleEndianCodec::readU64(word<8>(), 0); }
            void skip(uint32_t count) { require(count); offset += count; }
            [[nodiscard]] RangeInspectorReader subReader(uint32_t count) {
                require(count);
                const auto begin = offset; offset += count;
                return {read, checkpoint, begin, offset};
            }
            [[nodiscard]] Key readKey(uint32_t count) {
                require(count);
                const auto begin = offset; offset += count;
                return {begin, count};
            }
            [[nodiscard]] bool keyLess(Key left, Key right) const {
                std::array<std::byte, 256> a, b;
                const auto common = (std::min)(left.count, right.count);
                for (size_t position = 0; position < common;) {
                    if (checkpoint) checkpoint();
                    const auto count = (std::min)(a.size(), common - position);
                    read(left.offset + position, std::span(a).first(count));
                    read(right.offset + position, std::span(b).first(count));
                    for (size_t i = 0; i < count; ++i) {
                        if (a[i] != b[i]) return std::to_integer<unsigned>(a[i]) < std::to_integer<unsigned>(b[i]);
                    }
                    position += count;
                }
                return left.count < right.count;
            }
        };

		[[nodiscard]] PropertyValueWireKind kindFor(const PropertyValue &value) {
			return std::visit([]<typename T>(const T &) {
				if constexpr (std::is_same_v<T, std::monostate>) return PropertyValueWireKind::PVW_NULL;
				else if constexpr (std::is_same_v<T, bool>) return PropertyValueWireKind::PVW_BOOLEAN;
				else if constexpr (std::is_same_v<T, int64_t>) return PropertyValueWireKind::PVW_INTEGER;
				else if constexpr (std::is_same_v<T, double>) return PropertyValueWireKind::PVW_DOUBLE;
				else if constexpr (std::is_same_v<T, std::string>) return PropertyValueWireKind::PVW_STRING;
				else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) return PropertyValueWireKind::PVW_LIST;
				else if constexpr (std::is_same_v<T, PropertyValue::MapType>) return PropertyValueWireKind::PVW_MAP;
				else if constexpr (std::is_same_v<T, TemporalDate>) return PropertyValueWireKind::PVW_DATE;
				else if constexpr (std::is_same_v<T, TemporalDateTime>) return PropertyValueWireKind::PVW_DATETIME;
				else if constexpr (std::is_same_v<T, TemporalDuration>) return PropertyValueWireKind::PVW_DURATION;
				else { static_assert(std::is_same_v<T, PropertyValue::Bytes>); return PropertyValueWireKind::PVW_BYTES; }
			}, value.getVariant());
		}

		void encodeValue(std::vector<std::byte> &bytes, const PropertyValue &value, uint32_t depth);
		void encodeValue(std::pmr::vector<std::byte> &bytes, const PropertyValue &value, uint32_t depth);

		[[nodiscard]] size_t checkedAdd(size_t left, size_t right, const char *message) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return left + right;
		}

		[[nodiscard]] size_t encodedValueSize(const PropertyValue &value, uint32_t depth);

		[[nodiscard]] size_t encodedPayloadSize(const PropertyValue &value, uint32_t depth) {
			return std::visit(
					[depth]<typename T>(const T &argument) -> size_t {
						if constexpr (std::is_same_v<T, std::monostate>) {
							return 0U;
						} else if constexpr (std::is_same_v<T, bool>) {
							return 1U;
						} else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, double>) {
							return sizeof(uint64_t);
						} else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, PropertyValue::Bytes>) {
							requireU32Size(argument.size(), "Property value payload is too large");
							return checkedAdd(sizeof(uint32_t), argument.size(), "Property value payload size overflowed");
						} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
							requireU32Size(argument.size(), "Property list has too many elements");
							size_t result = sizeof(uint32_t);
							for (const PropertyValue &entry: argument) {
								result = checkedAdd(result, encodedValueSize(entry, depth + 1U),
														"Property list payload size overflowed");
							}
							return result;
						} else if constexpr (std::is_same_v<T, PropertyValue::MapType>) {
							requireU32Size(argument.size(), "Property map has too many entries");
							size_t result = sizeof(uint32_t);
							for (const auto &[key, entry]: argument) {
								requireU32Size(key.size(), "Property map key is too large");
								result = checkedAdd(result, sizeof(uint32_t), "Property map payload size overflowed");
								result = checkedAdd(result, key.size(), "Property map payload size overflowed");
								result = checkedAdd(result, encodedValueSize(entry, depth + 1U),
														"Property map payload size overflowed");
							}
							return result;
						} else if constexpr (std::is_same_v<T, TemporalDate>) {
							return sizeof(uint32_t);
						} else if constexpr (std::is_same_v<T, TemporalDateTime>) {
							return sizeof(uint64_t);
						} else if constexpr (std::is_same_v<T, TemporalDuration>) {
							return sizeof(uint64_t) * 3U;
						} else {
							static_assert(std::is_same_v<T, void>, "Unhandled PropertyValue variant");
						}
					},
					value.getVariant());
		}

		[[nodiscard]] size_t encodedValueSize(const PropertyValue &value, uint32_t depth) {
			if (depth > MAX_NESTING_DEPTH) {
				fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
			}
			const size_t payloadSize = encodedPayloadSize(value, depth);
			requireU32Size(payloadSize, "Property value payload is too large");
			return checkedAdd(6U, payloadSize, "Property value wire size overflowed");
		}

		template<class Bytes> void encodePayload(Bytes &payload, const PropertyValue &value, uint32_t depth) {
			std::visit([&payload, depth]<typename T>(const T &argument) {
				if constexpr (std::is_same_v<T, std::monostate>) {
					return;
				} else if constexpr (std::is_same_v<T, bool>) {
					appendU8(payload, argument ? 1U : 0U);
				} else if constexpr (std::is_same_v<T, int64_t>) {
					appendU64(payload, std::bit_cast<uint64_t>(argument));
				} else if constexpr (std::is_same_v<T, double>) {
					static_assert(std::numeric_limits<double>::is_iec559);
					appendU64(payload, std::bit_cast<uint64_t>(argument));
				} else if constexpr (std::is_same_v<T, std::string>) {
					requireU32Size(argument.size(), "Property string is too large");
					appendU32(payload, static_cast<uint32_t>(argument.size()));
					appendBytes(payload, argument);
				} else if constexpr (std::is_same_v<T, std::vector<PropertyValue>>) {
					requireU32Size(argument.size(), "Property list has too many elements");
					appendU32(payload, static_cast<uint32_t>(argument.size()));
					for (const auto &entry: argument) encodeValue(payload, entry, depth + 1U);
				} else if constexpr (std::is_same_v<T, PropertyValue::MapType>) {
					auto *resource = [&]() -> std::pmr::memory_resource * {
                        if constexpr (requires { payload.get_allocator().resource(); }) return payload.get_allocator().resource();
                        else return std::pmr::get_default_resource();
                    }();
                    std::pmr::vector<std::pair<std::string_view, const PropertyValue *>> entries(resource);
					entries.reserve(argument.size());
					for (const auto &[key, entry]: argument) entries.emplace_back(key, &entry);
					std::ranges::sort(entries, [](const auto &left, const auto &right) {
						return bytewiseLess(left.first, right.first);
					});
					requireU32Size(entries.size(), "Property map has too many entries");
					appendU32(payload, static_cast<uint32_t>(entries.size()));
					for (const auto &[key, entry]: entries) {
						requireU32Size(key.size(), "Property map key is too large");
						appendU32(payload, static_cast<uint32_t>(key.size()));
						appendBytes(payload, key);
						encodeValue(payload, *entry, depth + 1U);
					}
				} else if constexpr (std::is_same_v<T, TemporalDate>) {
					appendU32(payload, std::bit_cast<uint32_t>(argument.epochDays));
					} else if constexpr (std::is_same_v<T, TemporalDateTime>) {
						appendU64(payload, std::bit_cast<uint64_t>(argument.epochMillis));
					} else if constexpr (std::is_same_v<T, TemporalDuration>) {
						appendU64(payload, std::bit_cast<uint64_t>(argument.months));
						appendU64(payload, std::bit_cast<uint64_t>(argument.days));
						appendU64(payload, std::bit_cast<uint64_t>(argument.nanos));
					} else if constexpr (std::is_same_v<T, PropertyValue::Bytes>) {
						requireU32Size(argument.size(), "Property binary value is too large");
						appendU32(payload, static_cast<uint32_t>(argument.size()));
						appendBytes(payload, std::span<const std::byte>(argument.data(), argument.size()));
					} else {
						static_assert(std::is_same_v<T, void>, "Unhandled PropertyValue variant");
					}
			}, value.getVariant());
		}

		void encodeValue(std::vector<std::byte> &bytes, const PropertyValue &value, uint32_t depth) {
			if (depth > MAX_NESTING_DEPTH) fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
			std::vector<std::byte> payload;
			encodePayload(payload, value, depth);
			requireU32Size(payload.size(), "Property value payload is too large");
			appendU8(bytes, static_cast<uint8_t>(kindFor(value)));
			appendU8(bytes, 0U);
			appendU32(bytes, static_cast<uint32_t>(payload.size()));
			appendBytes(bytes, payload);
		}

		void encodeValue(std::pmr::vector<std::byte> &bytes, const PropertyValue &value, uint32_t depth) {
            if (depth > MAX_NESTING_DEPTH) fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
            const size_t start = bytes.size();
            try {
                appendU8(bytes, static_cast<uint8_t>(kindFor(value)));
                appendU8(bytes, 0U);
                appendU32(bytes, 0U);
                encodePayload(bytes, value, depth);
                const size_t payloadBytes = bytes.size() - start - CanonicalPropertyValueCodec::HEADER_BYTES;
                requireU32Size(payloadBytes, "Property value payload is too large");
                LittleEndianCodec::writeU32(bytes, start + 2U, static_cast<uint32_t>(payloadBytes));
            } catch (...) { bytes.resize(start); throw; }
        }

        using query::execution::detail::addStandardHeapBytes;
        using query::execution::detail::standardAllocatorRequestBytes;
        using query::execution::detail::standardContainerProxyBytes;
        size_t checkedHeapProduct(size_t count, size_t width) {
            if (width != 0U && count > (std::numeric_limits<size_t>::max)()/width)
                fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property decoded heap bound overflowed");
            return count * width;
        }
        size_t constructedStringHeapUpper(size_t count) {
            return utils::detail::standardFreshStringHeapUpper(count);
        }

        // libc++ reserve divides using float. Padding by at least one full ULP
        // prevents rounding below count before rehash, also at large wire counts.
        size_t decodedMapReserveCount(size_t count) {
            return property_copy::freshMapReserveCount(count);
        }
        size_t defaultMovingMapHeapUpper() {
#if defined(_MSVC_STL_VERSION)
            using Entry = PropertyValue::MapType::value_type;
            constexpr size_t node = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
            return addStandardHeapBytes(2U * standardContainerProxyBytes(),
                addStandardHeapBytes(standardAllocatorRequestBytes(node, alignof(Entry)),
                    standardAllocatorRequestBytes(16U * sizeof(void *), alignof(void *))));
#else
            return 0U;
#endif
        }
        size_t reservedDecodedMapHeapUpper(size_t count) {
            using Entry = PropertyValue::MapType::value_type;
            constexpr size_t node = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
#if defined(_MSVC_STL_VERSION)
            const auto nodes = addStandardHeapBytes(count, 1U);
            constexpr size_t bucketWidth = 2U * sizeof(void *);
            const auto buckets = (std::max)(size_t{8U}, checkedHeapProduct(decodedMapReserveCount(decodedMapReserveCount(count)), 2U));
#elif defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
            const auto nodes = count;
            constexpr size_t bucketWidth = sizeof(void *);
            // In the uint32 wire-payload domain, audited libstdc++ table gaps
            // and libc++ next-prime/power2
            // selection are <= twice a positive target. The second padding
            // bounds upward float rounding of reserve, as well as the first
            // padding preventing a rounded target below the element count.
            const auto buckets = checkedHeapProduct((std::max)(size_t{1U}, decodedMapReserveCount(decodedMapReserveCount(count))), 2U);
#else
            const size_t nodes = 0U, buckets = 0U;
            constexpr size_t bucketWidth = sizeof(void *);
            throw std::logic_error("admitted Property decoding requires a supported standard hash ABI");
#endif
            auto bytes = addStandardHeapBytes(2U * standardContainerProxyBytes(),
                addStandardHeapBytes(checkedHeapProduct(nodes, standardAllocatorRequestBytes(node, alignof(Entry))),
                    standardAllocatorRequestBytes(checkedHeapProduct(buckets, bucketWidth), alignof(void *))));
            // Two empty source maps from the by-value PropertyValue constructor
            // plus a parent/outer move; this also covers initial reserve overlap.
            return addStandardHeapBytes(bytes, checkedHeapProduct(3U, defaultMovingMapHeapUpper()));
        }
        template<class T> size_t decodedVectorHeapUpper(size_t count) {
            // The by-value PropertyValue constructor and a parent/outer move
            // leave up to three additional empty iterator-debug headers.
            return addStandardHeapBytes(4U * standardContainerProxyBytes(),
                standardAllocatorRequestBytes(checkedHeapProduct(count, sizeof(T)), alignof(T)));
        }

        class ShapeInspector final {
        public:
            explicit ShapeInspector(const std::function<void()> &checkpoint) : checkpoint_(checkpoint) {}
            CanonicalPropertyValueCodec::DecodeShape shape;
            template<class Reader> void scan(Reader &reader, uint32_t depth) {
                if (depth > MAX_NESTING_DEPTH) fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
                if (--until_checkpoint_ == 0U) { if (checkpoint_) checkpoint_(); until_checkpoint_ = 256U; }
                add(shape.value_count, 1U); shape.maximum_depth = (std::max)(shape.maximum_depth, size_t{depth});
                const auto kind = static_cast<PropertyValueWireKind>(reader.readU8());
                if (reader.readU8() != 0U) fail(FormatError::FERR_RESERVED_BITS, "Property value has nonzero flags");
                auto payload = reader.subReader(reader.readU32());
                switch (kind) {
                    case PropertyValueWireKind::PVW_NULL: break;
                    case PropertyValueWireKind::PVW_BOOLEAN:
                        if (payload.readU8() > 1U) fail(FormatError::FERR_INVALID_ENUM, "Property Boolean value is invalid");
                        break;
                    case PropertyValueWireKind::PVW_INTEGER:
                    case PropertyValueWireKind::PVW_DOUBLE:
                    case PropertyValueWireKind::PVW_DATETIME: static_cast<void>(payload.readU64()); break;
                    case PropertyValueWireKind::PVW_DATE: static_cast<void>(payload.readU32()); break;
                    case PropertyValueWireKind::PVW_DURATION:
                        for (size_t i = 0U; i < 3U; ++i) static_cast<void>(payload.readU64());
                        break;
                    case PropertyValueWireKind::PVW_STRING: {
                        const auto count = payload.readU32(); payload.skip(count); add(shape.string_bytes, count);
                        heap(addStandardHeapBytes(constructedStringHeapUpper(count), 2U * standardContainerProxyBytes())); break;
                    }
                    case PropertyValueWireKind::PVW_BYTES: {
                        const auto count = payload.readU32(); payload.skip(count); add(shape.binary_bytes, count);
                        heap(decodedVectorHeapUpper<std::byte>(count)); break;
                    }
                    case PropertyValueWireKind::PVW_LIST: {
                        const auto count = payload.readU32();
                        if (count > payload.remaining()/6U) fail(FormatError::FERR_INVALID_HEADER, "Property list count exceeds payload");
                        add(shape.list_elements, count); heap(decodedVectorHeapUpper<PropertyValue>(count));
                        for (uint32_t i = 0U; i < count; ++i) scan(payload, depth + 1U);
                        break;
                    }
                    case PropertyValueWireKind::PVW_MAP: {
                        const auto count = payload.readU32();
                        if (count > payload.remaining()/10U) fail(FormatError::FERR_INVALID_HEADER, "Property map count exceeds payload");
                        add(shape.map_entries, count); heap(reservedDecodedMapHeapUpper(count)); typename Reader::Key previous{};
                        for (uint32_t i = 0U; i < count; ++i) {
                            const auto keyBytes = payload.readU32();
                            const auto key = payload.readKey(keyBytes);
                            if (i != 0U && !payload.keyLess(previous, key))
                                fail(FormatError::FERR_INVALID_HEADER, "Property map keys are not canonical");
                            add(shape.map_key_bytes, keyBytes);
                            heap(addStandardHeapBytes(constructedStringHeapUpper(keyBytes), standardContainerProxyBytes()));
                            scan(payload, depth + 1U); previous = key;
                        }
                        break;
                    }
                    default: fail(FormatError::FERR_INVALID_ENUM, "Property value kind is unknown");
                }
                if (payload.remaining() != 0U) fail(FormatError::FERR_INVALID_HEADER, "Property payload has trailing bytes");
            }
        private:
            void heap(size_t count) { add(shape.maximum_heap_request_bytes, count); }
            static void add(size_t &target, size_t count) {
                if (count > (std::numeric_limits<size_t>::max)() - target)
                    fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "Property decode shape overflowed");
                target += count;
            }
            const std::function<void()> &checkpoint_;
            size_t until_checkpoint_ = 256U;
        };

        struct DecodeCheckpoints {
            const std::function<void()> &checkpoint;
            size_t remaining = 256U;
            void tick() {
                if (--remaining == 0U) { if (checkpoint) checkpoint(); remaining = 256U; }
            }
        };
        [[nodiscard]] PropertyValue decodeValue(ByteReader &reader, uint32_t depth,
                bool reserveMaps = false, DecodeCheckpoints *checkpoints = nullptr) {
            if (checkpoints) checkpoints->tick();
			if (depth > MAX_NESTING_DEPTH) fail(FormatError::FERR_INVALID_STATE, "Property value nesting is too deep");
			const auto kind = static_cast<PropertyValueWireKind>(reader.readU8());
			if (reader.readU8() != 0U) fail(FormatError::FERR_RESERVED_BITS, "Property value has nonzero flags");
			ByteReader payload{reader.readBytes(reader.readU32())};
			auto requireComplete = [&payload] {
				if (payload.remaining() != 0U) fail(FormatError::FERR_INVALID_HEADER, "Property payload has trailing bytes");
			};
			switch (kind) {
				case PropertyValueWireKind::PVW_NULL: requireComplete(); return PropertyValue(std::monostate{});
				case PropertyValueWireKind::PVW_BOOLEAN: {
					const auto value = payload.readU8();
					if (value > 1U) fail(FormatError::FERR_INVALID_ENUM, "Property Boolean value is invalid");
					requireComplete(); return PropertyValue(value != 0U);
				}
				case PropertyValueWireKind::PVW_INTEGER: {
					const auto value = std::bit_cast<int64_t>(payload.readU64()); requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_DOUBLE: {
					static_assert(std::numeric_limits<double>::is_iec559);
					const auto value = std::bit_cast<double>(payload.readU64()); requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_STRING: {
					auto value = payload.readString(payload.readU32()); requireComplete(); return PropertyValue(std::move(value));
				}
				case PropertyValueWireKind::PVW_LIST: {
					const uint32_t count = payload.readU32();
					if (count > payload.remaining() / 6U) fail(FormatError::FERR_INVALID_HEADER, "Property list count exceeds payload");
					std::vector<PropertyValue> values; values.reserve(count);
					for (uint32_t index = 0U; index < count; ++index) values.push_back(decodeValue(payload, depth + 1U, reserveMaps, checkpoints));
					requireComplete(); return PropertyValue(std::move(values));
				}
				case PropertyValueWireKind::PVW_MAP: {
					const uint32_t count = payload.readU32();
					if (count > payload.remaining() / 10U) fail(FormatError::FERR_INVALID_HEADER, "Property map count exceeds payload");
					PropertyValue::MapType values;
                    if (reserveMaps && count != 0U) values.reserve(decodedMapReserveCount(count));
                    std::string_view previous;
					for (uint32_t index = 0U; index < count; ++index) {
						const auto key = payload.readStringView(payload.readU32());
						if (index != 0U && !bytewiseLess(previous, key)) fail(FormatError::FERR_INVALID_HEADER, "Property map keys are not canonical");
						values.emplace(std::string(key), decodeValue(payload, depth + 1U, reserveMaps, checkpoints)); previous = key;
					}
					requireComplete(); return PropertyValue(std::move(values));
				}
				case PropertyValueWireKind::PVW_DATE: {
					const TemporalDate value{std::bit_cast<int32_t>(payload.readU32())}; requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_DATETIME: {
					const TemporalDateTime value{std::bit_cast<int64_t>(payload.readU64())}; requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_DURATION: {
					const TemporalDuration value{std::bit_cast<int64_t>(payload.readU64()), std::bit_cast<int64_t>(payload.readU64()), std::bit_cast<int64_t>(payload.readU64())};
					requireComplete(); return PropertyValue(value);
				}
				case PropertyValueWireKind::PVW_BYTES: {
					const uint32_t size = payload.readU32();
					const auto source = payload.readBytes(size);
					PropertyValue::Bytes value(source.begin(), source.end());
					requireComplete(); return PropertyValue(std::move(value));
				}
				default: fail(FormatError::FERR_INVALID_ENUM, "Property value kind is unknown");
			}
		}

	} // namespace

	bool CanonicalPropertyValueCodec::isNullHeader(std::span<const std::byte> header, const size_t encodedBytes) {
		if (header.size() < HEADER_BYTES || encodedBytes < HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "Property value header is truncated");
		}
		ByteReader reader{header.first(HEADER_BYTES)};
		const auto kind = static_cast<PropertyValueWireKind>(reader.readU8());
		if (reader.readU8() != 0U) fail(FormatError::FERR_RESERVED_BITS, "Property value has nonzero flags");
		const uint32_t payloadBytes = reader.readU32();
		if (kind < PropertyValueWireKind::PVW_NULL || kind > PropertyValueWireKind::PVW_BYTES ||
			payloadBytes != encodedBytes - HEADER_BYTES || (kind == PropertyValueWireKind::PVW_NULL && payloadBytes != 0U)) {
			fail(FormatError::FERR_INVALID_HEADER, "Property value header is invalid");
		}
		bool validSize = false;
		switch (kind) {
			case PropertyValueWireKind::PVW_NULL: validSize = payloadBytes == 0U; break;
			case PropertyValueWireKind::PVW_BOOLEAN: validSize = payloadBytes == 1U; break;
			case PropertyValueWireKind::PVW_INTEGER:
			case PropertyValueWireKind::PVW_DOUBLE:
			case PropertyValueWireKind::PVW_DATETIME: validSize = payloadBytes == 8U; break;
			case PropertyValueWireKind::PVW_DATE: validSize = payloadBytes == 4U; break;
			case PropertyValueWireKind::PVW_DURATION: validSize = payloadBytes == 24U; break;
			case PropertyValueWireKind::PVW_STRING:
			case PropertyValueWireKind::PVW_LIST:
			case PropertyValueWireKind::PVW_MAP:
			case PropertyValueWireKind::PVW_BYTES: validSize = payloadBytes >= 4U; break;
		}
		if (!validSize) fail(FormatError::FERR_INVALID_HEADER, "Property value header has an invalid type length");
		return kind == PropertyValueWireKind::PVW_NULL;
	}

    std::optional<CanonicalPropertyValueCodec::BorrowedPrimitive>
    CanonicalPropertyValueCodec::tryBorrowPrimitive(std::span<const std::byte> bytes) {
        static_cast<void>(isNullHeader(bytes, bytes.size()));
        const auto kind = static_cast<PropertyValueWireKind>(std::to_integer<uint8_t>(bytes.front()));
        ByteReader payload{bytes.subspan(HEADER_BYTES)};
        switch (kind) {
            case PropertyValueWireKind::PVW_NULL: return BorrowedPrimitive{};
            case PropertyValueWireKind::PVW_BOOLEAN: {
                const auto value = payload.readU8();
                if (value > 1U) fail(FormatError::FERR_INVALID_HEADER, "Property boolean is not canonical");
                return BorrowedPrimitive{PropertyType::BOOLEAN, value, {}};
            }
            case PropertyValueWireKind::PVW_INTEGER:
                return BorrowedPrimitive{PropertyType::INTEGER, payload.readU64(), {}};
            case PropertyValueWireKind::PVW_DOUBLE:
                return BorrowedPrimitive{PropertyType::DOUBLE, payload.readU64(), {}};
            case PropertyValueWireKind::PVW_STRING: {
                const auto size = payload.readU32();
                const auto text = payload.readStringView(size);
                if (payload.remaining() != 0U) fail(FormatError::FERR_INVALID_HEADER, "Property string has trailing bytes");
                return BorrowedPrimitive{PropertyType::STRING, 0U, text};
            }
            default: return std::nullopt;
        }
    }

	size_t CanonicalPropertyValueCodec::encodedSize(const PropertyValue &value) {
		return encodedValueSize(value, 0U);
	}

	void CanonicalPropertyValueCodec::append(std::pmr::vector<std::byte> &bytes, const PropertyValue &value) {
		encodeValue(bytes, value, 0U);
	}

	void CanonicalPropertyValueCodec::append(std::vector<std::byte> &bytes, const PropertyValue &value) {
		encodeValue(bytes, value, 0U);
	}

    size_t CanonicalPropertyValueCodec::guardedCopyHeapUpper(const PropertyValue &value,
            const std::function<void()> &checkpoint) {
        size_t bytes = 0U, remaining = 256U;
        if (checkpoint) checkpoint();
        const auto add = [&](size_t count) { bytes = addStandardHeapBytes(bytes, count); };
        const auto visit = [&](const auto &self, const PropertyValue &current, size_t depth) -> void {
            if (depth > MAX_NESTING_DEPTH) throw std::length_error("guarded Property copy nesting is too deep");
            if (--remaining == 0U) { remaining = 256U; if (checkpoint) checkpoint(); }
            const auto &variant = current.getVariant();
            if (const auto *text = std::get_if<std::string>(&variant)) {
                add(addStandardHeapBytes(constructedStringHeapUpper(text->size()), 2U * standardContainerProxyBytes()));
            } else if (const auto *binary = std::get_if<PropertyValue::Bytes>(&variant)) {
                add(decodedVectorHeapUpper<std::byte>(binary->size()));
            } else if (const auto *list = std::get_if<std::vector<PropertyValue>>(&variant)) {
                add(decodedVectorHeapUpper<PropertyValue>(list->size()));
                for (const auto &child : *list) self(self, child, depth + 1U);
            } else if (const auto *map = std::get_if<PropertyValue::MapType>(&variant)) {
                if (map->size() > std::numeric_limits<uint32_t>::max()/10U)
                    throw std::length_error("guarded Property map exceeds the audited wire-sized table domain");
                add(reservedDecodedMapHeapUpper(map->size()));
                for (const auto &[key, child] : *map) {
                    add(addStandardHeapBytes(constructedStringHeapUpper(key.size()), standardContainerProxyBytes()));
                    self(self, child, depth + 1U);
                }
            }
        };
        visit(visit, value, 0U);
        if (checkpoint) checkpoint();
        return bytes;
    }

    CanonicalPropertyValueCodec::DecodeShape CanonicalPropertyValueCodec::inspectNext(
            std::span<const std::byte> bytes, size_t &offset, const std::function<void()> &checkpoint) {
        if (offset > bytes.size()) fail(FormatError::FERR_INVALID_HEADER, "Property cursor exceeds payload");
        if (checkpoint) checkpoint();
        ByteReader reader{bytes.subspan(offset)};
        ShapeInspector inspector(checkpoint); inspector.scan(reader, 0U);
        if (checkpoint) checkpoint();
        inspector.shape.encoded_bytes = reader.offset;
        offset += reader.offset; return inspector.shape;
    }

    CanonicalPropertyValueCodec::DecodeShape CanonicalPropertyValueCodec::inspectRange(
            size_t encodedBytes, const RangeReader &read, const std::function<void()> &checkpoint) {
        if (!read) throw std::invalid_argument("Property range inspection requires a reader");
        if (checkpoint) checkpoint();
        RangeInspectorReader reader{read, checkpoint, 0, encodedBytes};
        ShapeInspector inspector(checkpoint); inspector.scan(reader, 0U);
        if (reader.remaining()) fail(FormatError::FERR_INVALID_HEADER, "Property range has trailing bytes");
        if (checkpoint) checkpoint();
        inspector.shape.encoded_bytes = encodedBytes;
        return inspector.shape;
    }

    PropertyValue CanonicalPropertyValueCodec::decodeNextWithAdmission(std::span<const std::byte> bytes,
            size_t &offset, const std::function<void(const DecodeShape &)> &admission,
            const std::function<void()> &checkpoint) {
        if (!admission) throw std::invalid_argument("Property decode requires its admission callback");
        size_t inspected = offset;
        const auto shape = inspectNext(bytes, inspected, checkpoint);
        admission(shape);
        if (checkpoint) checkpoint();
        ByteReader reader{bytes.subspan(offset)};
        DecodeCheckpoints decoding{checkpoint};
        PropertyValue value = decodeValue(reader, 0U, true, &decoding);
        offset += reader.offset;
        debug::PerfTrace::addValue("native_graph.read.materialized_bytes",
            reader.offset > static_cast<size_t>((std::numeric_limits<int64_t>::max)())
                ? (std::numeric_limits<int64_t>::max)() : static_cast<int64_t>(reader.offset));
        return value;
    }

	PropertyValue CanonicalPropertyValueCodec::decodeNext(std::span<const std::byte> bytes, size_t &offset) {
        if (auto *scope = property_decode::AllocationScope::current()) {
            const auto admit = [&](const DecodeShape &shape) { scope->admit(shape.maximum_heap_request_bytes); };
            const auto check = [&] { scope->checkpoint(); };
            // std::function reference_wrapper targets require no heap storage.
            return decodeNextWithAdmission(bytes, offset, std::ref(admit), std::ref(check));
        }
		if (offset > bytes.size()) fail(FormatError::FERR_INVALID_HEADER, "Property cursor exceeds payload");
		ByteReader reader{bytes.subspan(offset)};
		PropertyValue value = decodeValue(reader, 0U);
		offset += reader.offset;
		// Count consumed canonical bytes once at the outer value boundary.
		// This is decode work, not heap capacity or process RSS.
		debug::PerfTrace::addValue("native_graph.read.materialized_bytes",
			reader.offset > static_cast<size_t>(std::numeric_limits<int64_t>::max())
				? std::numeric_limits<int64_t>::max() : static_cast<int64_t>(reader.offset));
		return value;
	}

} // namespace graph::storage
