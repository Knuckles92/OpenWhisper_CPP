#include "cli/pe_imports.h"

#include <cstdint>
#include <fstream>
#include <iterator>

namespace fv::cli {
namespace {

// Offsets and sizes of the PE/COFF specification.
constexpr std::size_t kLfanewOffset = 0x3C;
constexpr std::size_t kCoffSize = 20;
constexpr std::uint16_t kPe32Magic = 0x10B;
constexpr std::uint16_t kPe32PlusMagic = 0x20B;
constexpr std::size_t kSectionSize = 40;
constexpr std::size_t kDescriptorSize = 20;
constexpr std::size_t kImportDirectory = 1;
// Bounds that keep a damaged image from making the walk long: no real image comes near them.
constexpr std::size_t kMaxDescriptors = 4096;
constexpr std::size_t kMaxNameBytes = 260;

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    bool has(std::size_t offset, std::size_t size) const {
        return offset <= bytes_.size() && size <= bytes_.size() - offset;
    }
    std::uint16_t u16(std::size_t offset) const {
        return static_cast<std::uint16_t>(byte(offset) | (byte(offset + 1) << 8));
    }
    std::uint32_t u32(std::size_t offset) const {
        return static_cast<std::uint32_t>(u16(offset)) |
               (static_cast<std::uint32_t>(u16(offset + 2)) << 16);
    }
    unsigned byte(std::size_t offset) const { return std::to_integer<unsigned>(bytes_[offset]); }
    std::size_t size() const { return bytes_.size(); }

private:
    std::span<const std::byte> bytes_;
};

struct Section {
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t raw_offset = 0;
};

} // namespace

std::optional<std::vector<std::string>> pe_imports(std::span<const std::byte> image) {
    const Reader in(image);
    if (!in.has(0, kLfanewOffset + 4) || in.byte(0) != 'M' || in.byte(1) != 'Z') {
        return std::nullopt;
    }
    const std::size_t pe = in.u32(kLfanewOffset);
    if (!in.has(pe, 4 + kCoffSize) || in.u32(pe) != 0x00004550) { // "PE\0\0"
        return std::nullopt;
    }
    const std::size_t coff = pe + 4;
    const std::size_t sections = in.u16(coff + 2);
    const std::size_t optional_size = in.u16(coff + 16);
    const std::size_t optional = coff + kCoffSize;
    if (!in.has(optional, optional_size) || optional_size < 2) {
        return std::nullopt;
    }
    const std::uint16_t magic = in.u16(optional);
    std::size_t count_offset = 0;
    if (magic == kPe32Magic) {
        count_offset = 92;
    } else if (magic == kPe32PlusMagic) {
        count_offset = 108;
    } else {
        return std::nullopt;
    }
    if (optional_size < count_offset + 4) {
        return std::nullopt;
    }
    const std::uint32_t header_size = in.u32(optional + 60);
    const std::uint32_t directories = in.u32(optional + count_offset);
    const std::size_t directory = optional + count_offset + 4 + 8 * kImportDirectory;
    if (directories <= kImportDirectory || directory + 8 > optional + optional_size) {
        return std::vector<std::string>{};
    }
    const std::uint32_t import_rva = in.u32(directory);
    if (import_rva == 0) {
        return std::vector<std::string>{};
    }

    const std::size_t table = optional + optional_size;
    if (!in.has(table, sections * kSectionSize)) {
        return std::nullopt;
    }
    std::vector<Section> section_list;
    for (std::size_t i = 0; i < sections; ++i) {
        const std::size_t at = table + i * kSectionSize;
        section_list.push_back(
            Section{in.u32(at + 12), in.u32(at + 8), in.u32(at + 16), in.u32(at + 20)});
    }
    // An RVA's file offset: inside a section's raw data, or in the headers, which are mapped as
    // they are in the file.
    const auto offset_of = [&](std::uint32_t rva) -> std::optional<std::size_t> {
        for (const Section& section : section_list) {
            const std::uint32_t span =
                section.virtual_size > section.raw_size ? section.virtual_size : section.raw_size;
            if (rva >= section.virtual_address && rva - section.virtual_address < span) {
                const std::uint32_t inside = rva - section.virtual_address;
                if (inside >= section.raw_size) {
                    return std::nullopt; // zero-filled in memory, absent from the file
                }
                return std::size_t{section.raw_offset} + inside;
            }
        }
        if (rva < header_size) {
            return std::size_t{rva};
        }
        return std::nullopt;
    };

    const std::optional<std::size_t> first = offset_of(import_rva);
    if (!first) {
        return std::nullopt;
    }
    std::vector<std::string> names;
    for (std::size_t i = 0;; ++i) {
        if (i == kMaxDescriptors) {
            return std::nullopt;
        }
        const std::size_t at = *first + i * kDescriptorSize;
        if (!in.has(at, kDescriptorSize)) {
            return std::nullopt;
        }
        const std::uint32_t name_rva = in.u32(at + 12);
        const std::uint32_t first_thunk = in.u32(at + 16);
        if (name_rva == 0 && first_thunk == 0) {
            break; // the null descriptor ends the table
        }
        const std::optional<std::size_t> name = offset_of(name_rva);
        if (!name) {
            return std::nullopt;
        }
        std::string text;
        for (std::size_t c = *name;; ++c) {
            if (c >= in.size() || text.size() > kMaxNameBytes) {
                return std::nullopt;
            }
            const unsigned value = in.byte(c);
            if (value == 0) {
                break;
            }
            text += static_cast<char>(value);
        }
        names.push_back(std::move(text));
    }
    return names;
}

std::optional<std::vector<std::string>> pe_imports(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return std::nullopt;
    }
    return pe_imports(std::as_bytes(std::span<const char>(bytes)));
}

} // namespace fv::cli
