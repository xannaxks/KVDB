#include "record.h"

//ByteRecord::ByteRecord(const InternalRecord& entry)
//	: key(entry.key), value(entry.value), type(entry.type)
//{
//}
//ByteRecord::ByteRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type)
//	: key_entry(key_entry), value_entry(value_entry), type(type)
//{}

InternalRecord::InternalRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num)
	: key_entry(key_entry), value_entry(value_entry), type(type), seq_num(seq_num)
{}
//
//
//std::vector<std::byte> InternalRecord::return_byte_sequence()
//{
//	std::vector<std::byte> out;
//	write_raw_bytes(out, &this->key_entry.size, sizeof(this->key_entry.size));
//	write_raw_bytes(out, &this->value_entry.size, sizeof(this->value_entry.size));
//	write_raw_bytes(out, &this->key_entry.data, this->key_entry.size);
//	write_raw_bytes(out, &this->value_entry.data, this->data_entry.size);
//	return out;
//}

uint32_t InternalRecord::disk_size()
{
	return key_entry.size + value_entry.size + sizeof(seq_num) + sizeof(type);
}

bool InternalRecord::write(std::ofstream& file) const
{
    if (!kvdb::endian::write_u64_le(file, seq_num))
        return false;

    if (!kvdb::endian::write_u8(file, static_cast<std::uint8_t>(type)))
        return false;

    const auto* key_bytes =
        reinterpret_cast<const std::byte*>(key_entry.data);

    const auto* value_bytes =
        reinterpret_cast<const std::byte*>(value_entry.data);

    if (!kvdb::endian::write_bytes_with_u32_size(
        file,
        std::span<const std::byte>(key_bytes, key_entry.size)))
        return false;

    if (!kvdb::endian::write_bytes_with_u32_size(
        file,
        std::span<const std::byte>(value_bytes, value_entry.size)))
        return false;

    return true;
}
std::optional<InternalRecord> InternalRecord::read(std::ifstream& file, Arena& arena)
{
    InternalRecord result;

    auto seq_num_opt = kvdb::endian::read_u64_le(file);
    if (!seq_num_opt.has_value())
        return std::nullopt;

    result.seq_num = *seq_num_opt;

    auto type_opt = kvdb::endian::read_u8(file);
    if (!type_opt.has_value())
        return std::nullopt;

    const auto type_raw = *type_opt;

    if (type_raw != static_cast<std::uint8_t>(Type::Put) &&
        type_raw != static_cast<std::uint8_t>(Type::Tombstone))
    {
        return std::nullopt;
    }

    result.type = static_cast<Type>(type_raw);

    auto key_bytes_opt = kvdb::endian::read_bytes_with_u32_size(file);
    if (!key_bytes_opt.has_value())
        return std::nullopt;

    auto value_bytes_opt = kvdb::endian::read_bytes_with_u32_size(file);
    if (!value_bytes_opt.has_value())
        return std::nullopt;

    result.key_entry = ArenaEntry::make_entry(
        arena,
        std::span<const std::byte>(key_bytes_opt->data(), key_bytes_opt->size())
    );

    result.value_entry = ArenaEntry::make_entry(
        arena,
        std::span<const std::byte>(value_bytes_opt->data(), value_bytes_opt->size())
    );

    return std::optional<InternalRecord>{std::move(result)};
}

bool InternalRecord::operator==(const InternalRecord& other) const noexcept
{
	return (
		this->key_entry == other.key_entry &&
		this->value_entry == other.value_entry &&
		this->type == other.type &&
		this->seq_num == other.seq_num
	);
}
InternalRecord& InternalRecord::operator=(const InternalRecord& other) noexcept
{
    if (this == &other)
        return *this;

    key_entry = other.key_entry;
    value_entry = other.value_entry;
    type = other.type;
    seq_num = other.seq_num;

    return *this;
}
InternalRecord& InternalRecord::operator=(InternalRecord&& other) noexcept
{
    if (this == &other)
        return *this;

    key_entry = std::move(other.key_entry);
    value_entry = std::move(other.value_entry);
    type = other.type;
    seq_num = other.seq_num;

    return *this;
}