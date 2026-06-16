#include "icon.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace th2 {
namespace {

struct Section {
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_pointer = 0;
    std::uint32_t raw_size = 0;
};

struct ResourceData {
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
};

std::uint16_t u16(std::span<const std::uint8_t> data, std::size_t offset)
{
    if (offset + 2 > data.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(
        data[offset] | (data[offset + 1] << 8));
}

std::uint32_t u32(std::span<const std::uint8_t> data, std::size_t offset)
{
    if (offset + 4 > data.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(
        data[offset] | (data[offset + 1] << 8)
        | (data[offset + 2] << 16) | (data[offset + 3] << 24));
}

std::optional<std::size_t> rva_to_offset(
    std::uint32_t rva, const std::vector<Section>& sections)
{
    for (const auto& section : sections) {
        const auto size = std::max(section.virtual_size, section.raw_size);
        if (rva >= section.virtual_address
            && rva < section.virtual_address + size) {
            return section.raw_pointer + (rva - section.virtual_address);
        }
    }
    return std::nullopt;
}

std::optional<ResourceData> find_resource(
    std::span<const std::uint8_t> file,
    const std::vector<Section>& sections,
    std::size_t resource_offset,
    std::uint16_t type,
    std::uint16_t id)
{
    auto find_entry = [&](std::size_t directory, std::uint16_t value)
        -> std::optional<std::size_t> {
        if (directory + 16 > file.size()) {
            return std::nullopt;
        }
        const auto named = u16(file, directory + 12);
        const auto ids = u16(file, directory + 14);
        auto entry = directory + 16 + static_cast<std::size_t>(named) * 8;
        for (std::uint16_t i = 0; i < ids; ++i, entry += 8) {
            if (entry + 8 > file.size()) {
                return std::nullopt;
            }
            if (u32(file, entry) == value) {
                return entry;
            }
        }
        return std::nullopt;
    };

    auto entry = find_entry(resource_offset, type);
    if (!entry) {
        return std::nullopt;
    }
    auto offset = u32(file, *entry + 4);
    if ((offset & 0x80000000U) == 0) {
        return std::nullopt;
    }
    const auto type_directory = resource_offset + (offset & 0x7fffffffU);
    entry = find_entry(type_directory, id);
    if (!entry) {
        return std::nullopt;
    }
    offset = u32(file, *entry + 4);
    if ((offset & 0x80000000U) == 0) {
        return std::nullopt;
    }
    const auto name_directory = resource_offset + (offset & 0x7fffffffU);
    if (name_directory + 24 > file.size()) {
        return std::nullopt;
    }
    const auto named = u16(file, name_directory + 12);
    const auto language_entry =
        name_directory + 16 + static_cast<std::size_t>(named) * 8;
    offset = u32(file, language_entry + 4);
    if ((offset & 0x80000000U) != 0) {
        return std::nullopt;
    }
    const auto data_entry = resource_offset + offset;
    const auto rva = u32(file, data_entry);
    const auto size = u32(file, data_entry + 4);
    if (!rva_to_offset(rva, sections)) {
        return std::nullopt;
    }
    return ResourceData{rva, size};
}

std::optional<std::uint16_t> first_resource_id(
    std::span<const std::uint8_t> file,
    std::size_t resource_offset,
    std::uint16_t type)
{
    if (resource_offset + 16 > file.size()) {
        return std::nullopt;
    }
    const auto named = u16(file, resource_offset + 12);
    const auto ids = u16(file, resource_offset + 14);
    auto entry = resource_offset + 16 + static_cast<std::size_t>(named) * 8;
    for (std::uint16_t i = 0; i < ids; ++i, entry += 8) {
        if (entry + 8 > file.size()) {
            return std::nullopt;
        }
        if (u32(file, entry) == type) {
            const auto offset = u32(file, entry + 4);
            if ((offset & 0x80000000U) == 0) {
                return std::nullopt;
            }
            const auto type_directory =
                resource_offset + (offset & 0x7fffffffU);
            if (type_directory + 24 > file.size()) {
                return std::nullopt;
            }
            const auto type_named = u16(file, type_directory + 12);
            const auto type_ids = u16(file, type_directory + 14);
            if (type_ids == 0) {
                return std::nullopt;
            }
            const auto id_entry =
                type_directory + 16
                + static_cast<std::size_t>(type_named) * 8;
            return static_cast<std::uint16_t>(u32(file, id_entry));
        }
    }
    return std::nullopt;
}

SurfacePtr decode_icon_dib(std::span<const std::uint8_t> data)
{
    if (data.size() < 40 || u32(data, 0) < 40) {
        return {};
    }
    const int width = static_cast<int>(u32(data, 4));
    const int stored_height = static_cast<int>(u32(data, 8));
    const int height = stored_height / 2;
    const auto planes = u16(data, 12);
    const auto bit_count = u16(data, 14);
    const auto compression = u32(data, 16);
    if (width <= 0 || height <= 0 || planes != 1 || compression != 0) {
        return {};
    }

    const auto header_size = u32(data, 0);
    const auto palette_entries =
        bit_count <= 8
            ? (u32(data, 32) != 0 ? u32(data, 32) : (1U << bit_count))
            : 0U;
    const auto xor_offset = header_size + palette_entries * 4;
    const auto xor_stride =
        ((static_cast<std::size_t>(width) * bit_count + 31) / 32) * 4;
    const auto and_stride = ((static_cast<std::size_t>(width) + 31) / 32) * 4;
    const auto and_offset = xor_offset + xor_stride * height;
    if (and_offset + and_stride * height > data.size()) {
        return {};
    }

    SurfacePtr surface(SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32));
    if (!surface) {
        return {};
    }
    auto* pixels = static_cast<std::uint8_t*>(surface->pixels);
    for (int y = 0; y < height; ++y) {
        const auto source_y = height - 1 - y;
        const auto xor_row = xor_offset + xor_stride * source_y;
        const auto and_row = and_offset + and_stride * source_y;
        auto* dst = pixels + static_cast<std::size_t>(y) * surface->pitch;
        for (int x = 0; x < width; ++x) {
            std::uint8_t blue = 0;
            std::uint8_t green = 0;
            std::uint8_t red = 0;
            std::uint8_t alpha = 255;
            if (bit_count == 32) {
                const auto offset = xor_row + static_cast<std::size_t>(x) * 4;
                blue = data[offset];
                green = data[offset + 1];
                red = data[offset + 2];
                alpha = data[offset + 3];
            } else if (bit_count == 24) {
                const auto offset = xor_row + static_cast<std::size_t>(x) * 3;
                blue = data[offset];
                green = data[offset + 1];
                red = data[offset + 2];
            } else if (bit_count == 8) {
                const auto index = data[xor_row + x];
                const auto offset = header_size + static_cast<std::size_t>(index) * 4;
                blue = data[offset];
                green = data[offset + 1];
                red = data[offset + 2];
            } else if (bit_count == 4) {
                const auto byte = data[xor_row + x / 2];
                const auto index = x % 2 == 0 ? byte >> 4 : byte & 0x0f;
                const auto offset = header_size + static_cast<std::size_t>(index) * 4;
                blue = data[offset];
                green = data[offset + 1];
                red = data[offset + 2];
            } else {
                return {};
            }
            const bool masked =
                (data[and_row + x / 8] & (0x80U >> (x % 8))) != 0;
            if (masked && bit_count != 32) {
                alpha = 0;
            }
            dst[x * 4 + 0] = red;
            dst[x * 4 + 1] = green;
            dst[x * 4 + 2] = blue;
            dst[x * 4 + 3] = alpha;
        }
    }
    return surface;
}

}  // namespace

SurfacePtr load_executable_icon(const std::filesystem::path& executable)
{
    std::ifstream input(executable, std::ios::binary);
    if (!input) {
        return {};
    }
    std::vector<std::uint8_t> file(
        std::istreambuf_iterator<char>(input), {});
    if (file.size() < 0x40 || u16(file, 0) != 0x5a4d) {
        return {};
    }
    const auto pe_offset = u32(file, 0x3c);
    if (pe_offset + 248 > file.size()
        || u32(file, pe_offset) != 0x00004550) {
        return {};
    }
    const auto section_count = u16(file, pe_offset + 6);
    const auto optional_size = u16(file, pe_offset + 20);
    const auto optional = pe_offset + 24;
    if (optional + optional_size > file.size() || u16(file, optional) != 0x10b) {
        return {};
    }
    const auto resource_rva = u32(file, optional + 96 + 2 * 8);
    if (resource_rva == 0) {
        return {};
    }
    std::vector<Section> sections;
    auto section_offset = optional + optional_size;
    for (std::uint16_t i = 0; i < section_count; ++i) {
        if (section_offset + 40 > file.size()) {
            return {};
        }
        sections.push_back(Section{
            u32(file, section_offset + 12),
            u32(file, section_offset + 8),
            u32(file, section_offset + 20),
            u32(file, section_offset + 16),
        });
        section_offset += 40;
    }
    const auto resource_offset = rva_to_offset(resource_rva, sections);
    if (!resource_offset) {
        return {};
    }

    const auto group_id = first_resource_id(file, *resource_offset, 14);
    if (!group_id) {
        return {};
    }
    const auto group =
        find_resource(file, sections, *resource_offset, 14, *group_id);
    if (!group) {
        return {};
    }
    const auto group_offset = rva_to_offset(group->rva, sections);
    if (!group_offset || *group_offset + group->size > file.size()
        || group->size < 6) {
        return {};
    }
    const auto group_data =
        std::span<const std::uint8_t>(file).subspan(*group_offset, group->size);
    const auto count = u16(group_data, 4);
    std::uint16_t selected_id = 0;
    int selected_score = -1;
    for (std::uint16_t i = 0; i < count; ++i) {
        const auto offset = 6 + static_cast<std::size_t>(i) * 14;
        if (offset + 14 > group_data.size()) {
            break;
        }
        const int width = group_data[offset] == 0 ? 256 : group_data[offset];
        const int height =
            group_data[offset + 1] == 0 ? 256 : group_data[offset + 1];
        const int bits = u16(group_data, offset + 6);
        const int score = width * height * std::max(bits, 1);
        if (score > selected_score) {
            selected_score = score;
            selected_id = u16(group_data, offset + 12);
        }
    }
    if (selected_id == 0) {
        return {};
    }
    const auto icon = find_resource(file, sections, *resource_offset, 3, selected_id);
    if (!icon) {
        return {};
    }
    const auto icon_offset = rva_to_offset(icon->rva, sections);
    if (!icon_offset || *icon_offset + icon->size > file.size()) {
        return {};
    }
    return decode_icon_dib(
        std::span<const std::uint8_t>(file).subspan(*icon_offset, icon->size));
}

}  // namespace th2
