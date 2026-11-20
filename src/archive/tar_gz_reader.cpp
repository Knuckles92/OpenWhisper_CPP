#include "archive/tar_gz_reader.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <map>
#include <string_view>
#include <system_error>

#include "archive/errors.h"

namespace fv::archive {

namespace {

constexpr std::size_t kBlock = 512;

// tarfile's InvalidHeaderError and friends, before tarfile decides whether they end the
// archive quietly or raise ReadError.
struct HeaderError {
    std::string message;
};

bool is_python_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r') || (c >= '\x1c' && c <= '\x1f');
}

std::string_view strip(std::string_view text) {
    while (!text.empty() && is_python_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_python_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// nts(): the bytes up to the first NUL.
std::string nts(std::string_view field) {
    return std::string(field.substr(0, field.find('\0')));
}

// int(text, base) for digits with optional sign, "0o"/"0x" prefix and single underscores.
std::optional<std::int64_t> parse_int(std::string_view text, int base) {
    text = strip(text);
    bool negative = false;
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        negative = text.front() == '-';
        text.remove_prefix(1);
    }
    if (base == 8 && text.size() > 2 && text[0] == '0' && (text[1] == 'o' || text[1] == 'O')) {
        text.remove_prefix(2);
        if (text.starts_with('_')) {
            text.remove_prefix(1);
        }
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    bool previous_digit = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '_') {
            if (!previous_digit || i + 1 == text.size()) {
                return std::nullopt;
            }
            previous_digit = false;
            continue;
        }
        if (c < '0' || c > '0' + base - 1) {
            return std::nullopt;
        }
        value = value * base + (c - '0');
        previous_digit = true;
    }
    return negative ? -value : value;
}

// nti(): an octal field, or GNU's base-256 form when the first byte is 0x80 or 0xff.
std::int64_t nti(std::string_view field) {
    const auto first = static_cast<unsigned char>(field.front());
    if (first == 0x80 || first == 0xFF) {
        std::int64_t n = 0;
        for (std::size_t i = 1; i < field.size(); ++i) {
            n = (n << 8) + static_cast<unsigned char>(field[i]);
        }
        if (first == 0xFF) {
            n = n - (std::int64_t{1} << (8 * (field.size() - 1)));
        }
        return n;
    }
    const auto text = nts(field);
    if (std::any_of(text.begin(), text.end(),
                    [](char c) { return static_cast<unsigned char>(c) >= 0x80; })) {
        throw HeaderError{"invalid header"};
    }
    if (strip(text).empty()) {
        return 0;
    }
    const auto value = parse_int(text, 8);
    if (!value) {
        throw HeaderError{"invalid header"};
    }
    return *value;
}

bool is_supported(char type) {
    return std::string_view("01234567LKS", 11).find(type) != std::string_view::npos || type == '\0';
}

bool is_gnu_type(char type) {
    return type == 'L' || type == 'K' || type == 'S';
}

std::int64_t blocks(std::int64_t size) {
    return (size + static_cast<std::int64_t>(kBlock) - 1) / static_cast<std::int64_t>(kBlock) *
           static_cast<std::int64_t>(kBlock);
}

void strip_trailing_slashes(std::string& name) {
    while (name.ends_with('/')) {
        name.pop_back();
    }
}

using PaxHeaders = std::map<std::string, std::string, std::less<>>;

} // namespace

struct TarGzReader::Impl {
    std::ifstream file;
    z_stream zs{};
    bool inflating = false;
    bool stream_done = false; // every gzip member fully inflated and no input left
    bool produced_any = false;
    std::array<unsigned char, 1 << 16> input{};
    std::array<char, 1 << 16> output{};
    std::size_t out_pos = 0;
    std::size_t out_len = 0;

    std::int64_t data_left = 0;
    std::int64_t padding_left = 0;
    std::size_t next_index = 0;
    bool started = false;
    bool finished = false;
    PaxHeaders global_pax;

    ~Impl() {
        if (inflating) {
            inflateEnd(&zs);
        }
    }

    bool refill_input() {
        file.read(reinterpret_cast<char*>(input.data()),
                  static_cast<std::streamsize>(input.size()));
        const auto got = static_cast<uInt>(file.gcount());
        zs.next_in = input.data();
        zs.avail_in = got;
        return got > 0;
    }

    // Inflates more output; false at the end of the last gzip member.
    bool inflate_more() {
        while (!stream_done) {
            if (zs.avail_in == 0 && !refill_input()) {
                if (!produced_any) {
                    stream_done = true;
                    return false;
                }
                throw BadArchive(
                    "Compressed file ended before the end-of-stream marker was reached");
            }
            zs.next_out = reinterpret_cast<Bytef*>(output.data());
            zs.avail_out = static_cast<uInt>(output.size());
            const int status = inflate(&zs, Z_NO_FLUSH);
            out_pos = 0;
            out_len = output.size() - zs.avail_out;
            if (status == Z_STREAM_END) {
                // Another gzip member may follow, as gzip.GzipFile reads them.
                if (zs.avail_in == 0 && !refill_input()) {
                    stream_done = true;
                } else {
                    inflateReset(&zs);
                }
            } else if (status != Z_OK && status != Z_BUF_ERROR) {
                if (!produced_any) {
                    throw BadArchive("not a gzip file");
                }
                throw BadArchive(std::string("zlib error: Error ") + std::to_string(status) +
                                 " while decompressing data" +
                                 (zs.msg != nullptr ? std::string(": ") + zs.msg : ""));
            }
            if (out_len > 0) {
                produced_any = true;
                return true;
            }
        }
        return out_len > out_pos;
    }

    // Up to `size` decompressed bytes; fewer only at the end of the archive.
    std::size_t raw_read(char* destination, std::size_t size) {
        std::size_t total = 0;
        while (total < size) {
            if (out_pos == out_len && !inflate_more()) {
                break;
            }
            const auto take = std::min(size - total, out_len - out_pos);
            std::copy_n(output.data() + out_pos, take, destination + total);
            out_pos += take;
            total += take;
        }
        return total;
    }

    void skip(std::int64_t size) {
        std::array<char, 8192> scratch{};
        while (size > 0) {
            const auto want = static_cast<std::size_t>(
                std::min<std::int64_t>(size, static_cast<std::int64_t>(scratch.size())));
            if (raw_read(scratch.data(), want) != want) {
                throw BadArchive("unexpected end of data");
            }
            size -= static_cast<std::int64_t>(want);
        }
    }

    enum class BlockStatus { Ok, Empty, Truncated, Zeros };

    BlockStatus read_block(std::array<char, kBlock>& block) {
        const auto got = raw_read(block.data(), kBlock);
        if (got == 0) {
            return BlockStatus::Empty;
        }
        if (got != kBlock) {
            return BlockStatus::Truncated;
        }
        if (std::all_of(block.begin(), block.end(), [](char c) { return c == '\0'; })) {
            return BlockStatus::Zeros;
        }
        return BlockStatus::Ok;
    }

    // TarInfo.frombuf.
    static TarMember from_block(const std::array<char, kBlock>& block) {
        const std::string_view buf(block.data(), block.size());
        const auto checksum = nti(buf.substr(148, 8));
        std::int64_t unsigned_sum = 256;
        std::int64_t signed_sum = 256;
        for (std::size_t i = 0; i < kBlock; ++i) {
            if (i >= 148 && i < 156) {
                continue;
            }
            unsigned_sum += static_cast<unsigned char>(buf[i]);
            signed_sum += static_cast<signed char>(buf[i]);
        }
        if (checksum != unsigned_sum && checksum != signed_sum) {
            throw HeaderError{"bad checksum"};
        }
        TarMember member;
        member.name = nts(buf.substr(0, 100));
        member.mode = nti(buf.substr(100, 8));
        nti(buf.substr(108, 8)); // uid
        nti(buf.substr(116, 8)); // gid
        member.size = nti(buf.substr(124, 12));
        member.mtime = static_cast<double>(nti(buf.substr(136, 12)));
        member.type = buf[156];
        member.linkname = nts(buf.substr(157, 100));
        nti(buf.substr(329, 8)); // devmajor
        nti(buf.substr(337, 8)); // devminor
        const auto prefix = nts(buf.substr(345, 155));
        if (member.type == TarMember::kOldRegular && member.name.ends_with('/')) {
            member.type = TarMember::kDirectory;
        }
        if (member.is_directory()) {
            strip_trailing_slashes(member.name);
        }
        if (!prefix.empty() && !is_gnu_type(member.type)) {
            member.name = prefix + "/" + member.name;
        }
        return member;
    }

    // fromtarfile() of the header that a GNU long name or a pax header announces: any failure
    // there is a SubsequentHeaderError, which tarfile always raises.
    TarMember subsequent() {
        std::array<char, kBlock> block{};
        switch (read_block(block)) {
        case BlockStatus::Empty:
            throw BadArchive("empty header");
        case BlockStatus::Truncated:
            throw BadArchive("truncated header");
        case BlockStatus::Zeros:
            throw BadArchive("end of file header");
        case BlockStatus::Ok:
            break;
        }
        try {
            return process(from_block(block));
        } catch (const HeaderError& error) {
            throw BadArchive(error.message);
        }
    }

    static void apply_pax(TarMember& member, const PaxHeaders& headers) {
        for (const auto& [keyword, value] : headers) {
            if (keyword == "GNU.sparse.name" || keyword == "GNU.sparse.size" ||
                keyword == "GNU.sparse.realsize") {
                throw BadArchive("sparse members are not supported");
            }
            if (keyword == "path") {
                member.name = value;
                strip_trailing_slashes(member.name);
            } else if (keyword == "linkpath") {
                member.linkname = value;
            } else if (keyword == "size") {
                member.size = parse_int(value, 10).value_or(0);
            } else if (keyword == "mtime") {
                const auto text = strip(value);
                double mtime = 0;
                const auto [end, error] =
                    std::from_chars(text.data(), text.data() + text.size(), mtime);
                member.mtime =
                    (error == std::errc() && end == text.data() + text.size()) ? mtime : 0;
            }
        }
    }

    // _proc_member: a GNU long name or link, a pax header, or a plain member.
    TarMember process(TarMember member) {
        if (member.type == 'L' || member.type == 'K') {
            std::string data(static_cast<std::size_t>(blocks(member.size)), '\0');
            data.resize(raw_read(data.data(), data.size()));
            TarMember next = subsequent();
            if (member.type == 'L') {
                next.name = nts(data);
            } else {
                next.linkname = nts(data);
            }
            if (next.is_directory() && next.name.ends_with('/')) {
                next.name.pop_back();
            }
            return next;
        }
        if (member.type == 'S') {
            throw BadArchive("sparse members are not supported");
        }
        if (member.type == 'x' || member.type == 'g' || member.type == 'X') {
            std::string buffer(static_cast<std::size_t>(blocks(member.size)), '\0');
            buffer.resize(raw_read(buffer.data(), buffer.size()));
            PaxHeaders local = member.type == 'g' ? PaxHeaders{} : global_pax;
            PaxHeaders& target = member.type == 'g' ? global_pax : local;
            std::size_t pos = 0;
            while (buffer.size() > pos && buffer[pos] != '\0') {
                std::size_t digits = 0;
                while (pos + digits < buffer.size() && digits < 20 && buffer[pos + digits] >= '0' &&
                       buffer[pos + digits] <= '9') {
                    ++digits;
                }
                if (digits == 0 || pos + digits >= buffer.size() || buffer[pos + digits] != ' ') {
                    throw HeaderError{"invalid header"};
                }
                std::size_t length = 0;
                std::from_chars(buffer.data() + pos, buffer.data() + pos + digits, length);
                if (length < 5 || pos + length > buffer.size()) {
                    throw HeaderError{"invalid header"};
                }
                const auto end = pos + length - 1;
                const std::string_view record(buffer.data() + pos + digits + 1,
                                              end - (pos + digits + 1));
                const auto equals = record.find('=');
                if (equals == std::string_view::npos || equals == 0 || buffer[end] != '\n') {
                    throw HeaderError{"invalid header"};
                }
                target[std::string(record.substr(0, equals))] =
                    std::string(record.substr(equals + 1));
                pos += length;
            }
            for (const auto& [keyword, value] : target) {
                if (keyword.starts_with("GNU.sparse.")) {
                    throw BadArchive("sparse members are not supported");
                }
            }
            TarMember next = subsequent();
            if (member.type != 'g') {
                apply_pax(next, local);
                set_data(next);
            }
            return next;
        }
        // _proc_builtin
        apply_pax(member, global_pax);
        if (member.is_directory()) {
            strip_trailing_slashes(member.name);
        }
        set_data(member);
        return member;
    }

    void set_data(const TarMember& member) {
        if (member.is_regular() || !is_supported(member.type)) {
            data_left = member.size;
            padding_left = blocks(member.size) - member.size;
        } else {
            data_left = 0;
            padding_left = 0;
        }
    }

    std::optional<TarMember> next() {
        if (finished) {
            return std::nullopt;
        }
        skip(data_left + padding_left);
        data_left = 0;
        padding_left = 0;
        const bool first = !started;
        started = true;
        std::array<char, kBlock> block{};
        const auto status = read_block(block);
        if (status == BlockStatus::Empty) {
            if (first) {
                throw BadArchive("empty file");
            }
            finished = true;
            return std::nullopt;
        }
        if (status == BlockStatus::Truncated) {
            if (first) {
                throw BadArchive("truncated header");
            }
            finished = true;
            return std::nullopt;
        }
        if (status == BlockStatus::Zeros) {
            finished = true;
            return std::nullopt;
        }
        try {
            TarMember member = process(from_block(block));
            member.index = next_index++;
            return member;
        } catch (const HeaderError& error) {
            if (first) {
                throw BadArchive(error.message);
            }
            finished = true;
            return std::nullopt;
        }
    }
};

TarGzReader::TarGzReader(const std::filesystem::path& archive) : impl_(std::make_unique<Impl>()) {
    impl_->file.open(archive, std::ios::binary);
    if (!impl_->file) {
        throw std::filesystem::filesystem_error(
            "open", archive, std::make_error_code(std::errc::no_such_file_or_directory));
    }
    // 16 + MAX_WBITS: a gzip wrapper, as tarfile's "r:gz" accepts and nothing else.
    if (inflateInit2(&impl_->zs, 16 + MAX_WBITS) != Z_OK) {
        throw BadArchive("not a gzip file");
    }
    impl_->inflating = true;
}

TarGzReader::~TarGzReader() = default;

std::optional<TarMember> TarGzReader::next() {
    return impl_->next();
}

std::size_t TarGzReader::read(std::span<char> buffer) {
    const auto want = static_cast<std::size_t>(
        std::min<std::int64_t>(impl_->data_left, static_cast<std::int64_t>(buffer.size())));
    const auto got = impl_->raw_read(buffer.data(), want);
    if (got != want) {
        throw BadArchive("unexpected end of data");
    }
    impl_->data_left -= static_cast<std::int64_t>(got);
    return got;
}

std::vector<TarMember> list_tar_gz(const std::filesystem::path& archive) {
    TarGzReader reader(archive);
    std::vector<TarMember> members;
    while (auto member = reader.next()) {
        members.push_back(std::move(*member));
    }
    return members;
}

} // namespace fv::archive
