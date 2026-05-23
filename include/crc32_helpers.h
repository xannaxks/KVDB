#include <cstdint>
#include <bitset>
#include <utility>
#include "endian_io.h"
#include <type_traits>

#define Z_NULL nullptr

inline void compute_crc32(std::uint32_t& crc, const void* ptr, std::size_t size);
inline std::uint32_t crc32_of(const void* ptr, std::size_t size);
inline void compute_span_crc32(std::uint32_t& crc, const std::span<const std::byte>& value);

template <typename T>
inline void crc32_add_pod(std::uint32_t& crc, const T& value)
{
	std::vector<std::byte> byte_span;
	if constexpr (std::is_same_v<T, std::uint8_t>)
		kvdb::endian::put_u8(byte_span, value);
	if constexpr (std::is_same_v<T, std::uint16_t>)
		kvdb::endian::put_u16(byte_span, value);
	if constexpr (std::is_same_v<T, std::uint32_t>)
		kvdb::endian::put_u32(byte_span, value);
	if constexpr (std::is_same_v<T, std::uint64_t>)
		kvdb::endian::put_u64(byte_span, value);
	if constexpr (std::is_same_v<T, std::string>)
		kvdb::endian::put_bytes_with_u32_size(byte_span, value)

		compute_crc32(crc, &value, sizeof(T));
}

int crc32(std::uint64_t val, void*, std::size_t size)
{
	return 0;
}
