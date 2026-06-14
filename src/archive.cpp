#include "archive.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace th2 {
namespace {

std::uint32_t read_u32(std::istream& input)
{
    std::array<unsigned char, 4> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input) {
        throw std::runtime_error("unexpected end of archive");
    }
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::uint32_t read_u32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

bool ascii_iequals(std::string_view left, std::string_view right)
{
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a))
                   == std::tolower(static_cast<unsigned char>(b));
           });
}

std::vector<std::uint8_t> decompress_lzs(
    std::span<const std::uint8_t> source, std::size_t output_size)
{
    constexpr std::size_t ring_size = 4096;
    constexpr std::size_t lookahead = 18;
    std::array<std::uint8_t, ring_size> ring{};
    ring.fill(' ');
    std::size_t ring_position = ring_size - lookahead;
    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    std::size_t source_position = 0;
    unsigned flags = 0;

    while (source_position < source.size() && output.size() < output_size) {
        flags >>= 1;
        if ((flags & 0x100) == 0) {
            flags = source[source_position++] | 0xff00;
        }
        if (source_position >= source.size()) {
            break;
        }

        int first = source[source_position++];
        if (flags & 1) {
            const auto value = static_cast<std::uint8_t>(first);
            output.push_back(value);
            ring[ring_position++ & (ring_size - 1)] = value;
        } else {
            if (source_position >= source.size()) {
                break;
            }
            const int second = source[source_position++];
            int position = first | ((second & 0xf0) << 4);
            const int length = (second & 0x0f) + 3;
            for (int i = 0; i < length && output.size() < output_size; ++i) {
                const auto value = ring[position++ & (ring_size - 1)];
                output.push_back(value);
                ring[ring_position++ & (ring_size - 1)] = value;
            }
        }
    }

    if (output.size() != output_size) {
        throw std::runtime_error("LZS stream produced an unexpected size");
    }
    return output;
}

std::string fixed_string(const char* data, std::size_t size)
{
    std::size_t length = 0;
    while (length < size) {
        const auto byte = static_cast<unsigned char>(data[length]);
        if (byte == 0 || byte == 0xff) {
            break;
        }
        ++length;
    }
    return std::string(data, length);
}

void validate_entry(
    const ArchiveEntry& entry, std::uintmax_t archive_size,
    const std::filesystem::path& path)
{
    const auto end = static_cast<std::uintmax_t>(entry.offset) + entry.stored_size;
    if (end > archive_size) {
        throw std::runtime_error(
            path.string() + ": entry extends beyond end of archive: " + entry.name);
    }
}

}  // namespace

Archive::Archive(const std::filesystem::path& path)
    : path_(path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open " + path.string());
    }

    std::array<char, 4> magic{};
    input.read(magic.data(), magic.size());
    const std::uint32_t count = read_u32(input);
    const auto archive_size = std::filesystem::file_size(path);
    entries_.reserve(count);

    if (magic == std::array<char, 4>{'K', 'C', 'A', 'P'}) {
        kind_ = ArchiveKind::kcap;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t type = read_u32(input);
            std::array<char, 24> name{};
            input.read(name.data(), name.size());
            const std::uint32_t offset = read_u32(input);
            const std::uint32_t size = read_u32(input);
            if (!input) {
                throw std::runtime_error(path.string() + ": truncated KCAP directory");
            }
            ArchiveEntry entry{fixed_string(name.data(), name.size()), offset, size, type != 0};
            validate_entry(entry, archive_size, path);
            entries_.push_back(std::move(entry));
        }
    } else if (magic == std::array<char, 4>{'L', 'A', 'C', '\0'}) {
        kind_ = ArchiveKind::lac;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::array<char, 31> encoded_name{};
            input.read(encoded_name.data(), encoded_name.size());
            const int compressed = input.get();
            const std::uint32_t size = read_u32(input);
            const std::uint32_t offset = read_u32(input);
            if (!input) {
                throw std::runtime_error(path.string() + ": truncated LAC directory");
            }
            for (char& byte : encoded_name) {
                byte = static_cast<char>(~static_cast<unsigned char>(byte));
            }
            ArchiveEntry entry{
                fixed_string(encoded_name.data(), encoded_name.size()),
                offset,
                size,
                compressed != 0,
            };
            validate_entry(entry, archive_size, path);
            entries_.push_back(std::move(entry));
        }
    } else {
        throw std::runtime_error(path.string() + ": unsupported archive magic");
    }
}

const ArchiveEntry* Archive::find(std::string_view name) const
{
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return ascii_iequals(entry.name, name);
    });
    return found == entries_.end() ? nullptr : &*found;
}

std::vector<std::uint8_t> Archive::read(const ArchiveEntry& entry) const
{
    std::ifstream input(path_, std::ios::binary);
    input.seekg(entry.offset);
    std::vector<std::uint8_t> stored(entry.stored_size);
    input.read(reinterpret_cast<char*>(stored.data()), stored.size());
    if (!input) {
        throw std::runtime_error(path_.string() + ": cannot read " + entry.name);
    }
    if (!entry.compressed) {
        return stored;
    }
    if (kind_ == ArchiveKind::lac) {
        if (stored.size() < 4) {
            throw std::runtime_error(path_.string() + ": invalid compressed LAC entry");
        }
        return decompress_lzs(
            std::span<const std::uint8_t>(stored).subspan(4), read_u32(stored.data()));
    }
    if (stored.size() < 8) {
        throw std::runtime_error(path_.string() + ": invalid compressed KCAP entry");
    }
    return decompress_lzs(
        std::span<const std::uint8_t>(stored).subspan(8), read_u32(stored.data() + 4));
}

}  // namespace th2
