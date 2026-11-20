#include "meeting_intel/detail/office_text.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "archive/zip_reader.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting_intel::detail {

namespace fs = std::filesystem;

namespace {

namespace ns {
constexpr std::string_view kWord = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr std::string_view kDrawing = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view kPresentation =
    "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view kSheet = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::string_view kOfficeRels =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view kPackageRels =
    "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr std::string_view kContentTypes =
    "http://schemas.openxmlformats.org/package/2006/content-types";
} // namespace ns

namespace rel {
constexpr std::string_view kOfficeDocument =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument";
} // namespace rel

namespace content {
constexpr std::string_view kWordMain =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";
constexpr std::array<std::string_view, 2> kPresentationMains = {
    "application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml",
    "application/vnd.ms-powerpoint.presentation.macroEnabled.main+xml"};
// _find_workbook_part's order.
constexpr std::array<std::string_view, 4> kWorkbookMains = {
    "application/vnd.ms-excel.template.macroEnabled.main+xml",
    "application/vnd.openxmlformats-officedocument.spreadsheetml.template.main+xml",
    "application/vnd.ms-excel.sheet.macroEnabled.main+xml",
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"};
constexpr std::string_view kSharedStrings =
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml";
} // namespace content

// Uncompressed bytes one XML part may take: a damaged or hostile package is refused rather than
// inflated without bound (a document's text is clipped to 50,000 characters anyway).
constexpr std::size_t kMaxPartBytes = 256 * 1024 * 1024;

// ---- XML with namespaces (pugixml keeps prefixes; lxml compares namespace URIs) ----

std::string_view local_name(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string_view namespace_of(const pugi::xml_node& node) {
    const std::string_view name = node.name();
    const auto colon = name.find(':');
    const std::string attribute =
        colon == std::string_view::npos ? "xmlns" : "xmlns:" + std::string(name.substr(0, colon));
    for (pugi::xml_node current = node; current; current = current.parent()) {
        if (const pugi::xml_attribute declared = current.attribute(attribute.c_str())) {
            return declared.value();
        }
    }
    return {};
}

bool is(const pugi::xml_node& node, std::string_view uri, std::string_view local) {
    return node.type() == pugi::node_element && local_name(node) == local &&
           namespace_of(node) == uri;
}

std::vector<pugi::xml_node> children(const pugi::xml_node& node, std::string_view uri,
                                     std::string_view local) {
    std::vector<pugi::xml_node> found;
    for (const pugi::xml_node child : node.children()) {
        if (is(child, uri, local)) {
            found.push_back(child);
        }
    }
    return found;
}

pugi::xml_node child(const pugi::xml_node& node, std::string_view uri, std::string_view local) {
    for (const pugi::xml_node item : node.children()) {
        if (is(item, uri, local)) {
            return item;
        }
    }
    return {};
}

// An attribute by namespace and local name (an unprefixed attribute has no namespace).
pugi::xml_attribute attribute(const pugi::xml_node& node, std::string_view uri,
                              std::string_view local) {
    for (const pugi::xml_attribute item : node.attributes()) {
        const std::string_view name = item.name();
        const auto colon = name.find(':');
        if (colon == std::string_view::npos) {
            if (uri.empty() && name == local) {
                return item;
            }
            continue;
        }
        if (name.substr(colon + 1) != local) {
            continue;
        }
        const std::string declaration = "xmlns:" + std::string(name.substr(0, colon));
        for (pugi::xml_node current = node; current; current = current.parent()) {
            if (const pugi::xml_attribute declared = current.attribute(declaration.c_str())) {
                if (declared.value() == uri) {
                    return item;
                }
                break;
            }
        }
    }
    return {};
}

// lxml's element.text: the character data before the first child that is not text.
std::string element_text(const pugi::xml_node& node) {
    std::string text;
    for (const pugi::xml_node item : node.children()) {
        if (item.type() != pugi::node_pcdata && item.type() != pugi::node_cdata) {
            break;
        }
        text += item.value();
    }
    return text;
}

// ---- The package: zip entries, relationships and content types ----

struct Relationship {
    std::string id;
    std::string type;
    std::string target; // the part name it resolves to, without a leading "/"
    bool external = false;
};

std::string directory_of(std::string_view part) {
    const auto slash = part.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(part.substr(0, slash));
}

// posixpath.normpath(join(base, target)), or the target without its "/" when it is absolute.
std::string resolve_target(std::string_view base, std::string_view target) {
    std::string joined = target.starts_with('/') ? std::string(target.substr(1))
                         : base.empty()          ? std::string(target)
                                                 : std::string(base) + "/" + std::string(target);
    std::vector<std::string> parts;
    for (const auto& part : py::split_on(joined, "/")) {
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (const auto& part : parts) {
        out += (out.empty() ? "" : "/") + part;
    }
    return out;
}

class Package {
public:
    explicit Package(const fs::path& path) : zip_(path) {
        for (const auto& entry : zip_.entries()) {
            names_.insert(entry.name);
        }
    }

    bool has(std::string_view name) const { return names_.contains(name); }

    std::string read(std::string_view name) {
        std::string data;
        zip_.read_entry(name, [&data](std::span<const std::byte> block) {
            if (data.size() + block.size() > kMaxPartBytes) {
                throw std::runtime_error("a document part is too large");
            }
            data.append(reinterpret_cast<const char*>(block.data()), block.size());
        });
        return data;
    }

    pugi::xml_document xml(std::string_view name) {
        const std::string data = read(name);
        pugi::xml_document document;
        const pugi::xml_parse_result parsed =
            document.load_buffer(data.data(), data.size(),
                                 pugi::parse_default | pugi::parse_ws_pcdata, pugi::encoding_auto);
        if (!parsed) {
            throw std::runtime_error("a document part is not valid XML");
        }
        return document;
    }

    // The relationships of `part` ("" for the package's own), in document order.
    std::vector<Relationship> relationships(std::string_view part) {
        const std::string folder = directory_of(part);
        const std::string file = part.empty() ? "" : std::string(part.substr(part.rfind('/') + 1));
        const std::string rels_name =
            (folder.empty() ? "" : folder + "/") + "_rels/" + file + ".rels";
        std::vector<Relationship> found;
        if (!has(rels_name)) {
            return found;
        }
        const pugi::xml_document document = xml(rels_name);
        const pugi::xml_node root = child(document, ns::kPackageRels, "Relationships");
        for (const pugi::xml_node item : children(root, ns::kPackageRels, "Relationship")) {
            Relationship relationship;
            relationship.id = item.attribute("Id").value();
            relationship.type = item.attribute("Type").value();
            relationship.external =
                std::string_view(item.attribute("TargetMode").value()) == "External";
            relationship.target = relationship.external
                                      ? std::string(item.attribute("Target").value())
                                      : resolve_target(folder, item.attribute("Target").value());
            found.push_back(std::move(relationship));
        }
        return found;
    }

    // The part the package's officeDocument relationship names.
    std::string main_part() {
        for (const auto& relationship : relationships("")) {
            if (relationship.type == rel::kOfficeDocument && !relationship.external) {
                return relationship.target;
            }
        }
        throw std::runtime_error("the package has no main document");
    }

    // [Content_Types].xml: the Override of "/<part>", else the Default of its extension.
    std::string content_type(std::string_view part) {
        load_types();
        const auto override_type = overrides_.find("/" + std::string(part));
        if (override_type != overrides_.end()) {
            return override_type->second;
        }
        const auto dot = part.rfind('.');
        if (dot != std::string_view::npos) {
            const auto found = defaults_.find(py::lower(part.substr(dot + 1)));
            if (found != defaults_.end()) {
                return found->second;
            }
        }
        return "";
    }

    // The part name of the first Override with `type` ("" when none).
    std::string find_override(std::string_view type) {
        load_types();
        for (const auto& [name, value] : override_order_) {
            if (value == type) {
                return name;
            }
        }
        return "";
    }

    // Whether a Default declares `type`.
    bool default_declares(std::string_view type) {
        load_types();
        return std::any_of(defaults_.begin(), defaults_.end(),
                           [&](const auto& entry) { return entry.second == type; });
    }

private:
    void load_types() {
        if (types_loaded_) {
            return;
        }
        types_loaded_ = true;
        const pugi::xml_document document = xml("[Content_Types].xml");
        const pugi::xml_node root = child(document, ns::kContentTypes, "Types");
        for (const pugi::xml_node item : root.children()) {
            if (is(item, ns::kContentTypes, "Override")) {
                const std::string name = item.attribute("PartName").value();
                overrides_[name] = item.attribute("ContentType").value();
                override_order_.emplace_back(name, item.attribute("ContentType").value());
            } else if (is(item, ns::kContentTypes, "Default")) {
                defaults_[py::lower(item.attribute("Extension").value())] =
                    item.attribute("ContentType").value();
            }
        }
    }

    archive::ZipReader zip_;
    std::set<std::string, std::less<>> names_;
    bool types_loaded_ = false;
    std::map<std::string, std::string, std::less<>> overrides_;
    std::vector<std::pair<std::string, std::string>> override_order_;
    std::map<std::string, std::string, std::less<>> defaults_;
};

std::string join_lines(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            out += separator;
        }
        out += parts[index];
    }
    return out;
}

std::int64_t parse_int(std::string_view text) {
    const auto value = py::py_int(std::string(text));
    if (!value) {
        throw std::runtime_error("a document number is not an integer");
    }
    return *value;
}

// ---- DOCX ----

std::string run_text(const pugi::xml_node& run) {
    std::string text;
    for (const pugi::xml_node item : run.children()) {
        if (item.type() != pugi::node_element || namespace_of(item) != ns::kWord) {
            continue;
        }
        const std::string_view name = local_name(item);
        if (name == "t") {
            text += element_text(item);
        } else if (name == "tab" || name == "ptab") {
            text += '\t';
        } else if (name == "cr") {
            text += '\n';
        } else if (name == "noBreakHyphen") {
            text += '-';
        } else if (name == "br") {
            const pugi::xml_attribute type = attribute(item, ns::kWord, "type");
            if (!type || std::string_view(type.value()) == "textWrapping") {
                text += '\n';
            }
        }
    }
    return text;
}

std::string paragraph_text(const pugi::xml_node& paragraph) {
    std::string text;
    for (const pugi::xml_node item : paragraph.children()) {
        if (is(item, ns::kWord, "r")) {
            text += run_text(item);
        } else if (is(item, ns::kWord, "hyperlink")) {
            for (const pugi::xml_node run : children(item, ns::kWord, "r")) {
                text += run_text(run);
            }
        }
    }
    return text;
}

std::string cell_text(const pugi::xml_node& cell) {
    std::vector<std::string> paragraphs;
    for (const pugi::xml_node paragraph : children(cell, ns::kWord, "p")) {
        paragraphs.push_back(paragraph_text(paragraph));
    }
    return join_lines(paragraphs, "\n");
}

std::int64_t word_val(const pugi::xml_node& properties, std::string_view element,
                      std::int64_t fallback) {
    const pugi::xml_node node = child(properties, ns::kWord, element);
    if (!node) {
        return fallback;
    }
    const pugi::xml_attribute value = attribute(node, ns::kWord, "val");
    return value ? parse_int(value.value()) : fallback;
}

std::int64_t grid_span(const pugi::xml_node& cell) {
    const pugi::xml_node properties = child(cell, ns::kWord, "tcPr");
    return properties ? word_val(properties, "gridSpan", 1) : 1;
}

std::int64_t grid_before(const pugi::xml_node& row) {
    const pugi::xml_node properties = child(row, ns::kWord, "trPr");
    return properties ? word_val(properties, "gridBefore", 0) : 0;
}

bool continues_merge(const pugi::xml_node& cell) {
    const pugi::xml_node properties = child(cell, ns::kWord, "tcPr");
    if (!properties) {
        return false;
    }
    const pugi::xml_node merge = child(properties, ns::kWord, "vMerge");
    if (!merge) {
        return false;
    }
    const pugi::xml_attribute value = attribute(merge, ns::kWord, "val");
    return !value || std::string_view(value.value()) == "continue";
}

pugi::xml_node row_above(const pugi::xml_node& row) {
    for (pugi::xml_node previous = row.previous_sibling(); previous;
         previous = previous.previous_sibling()) {
        if (is(previous, ns::kWord, "tr")) {
            return previous;
        }
    }
    throw std::runtime_error("no tr above topmost tr in w:tbl");
}

pugi::xml_node cell_at_grid_offset(const pugi::xml_node& row, std::int64_t offset) {
    std::int64_t remaining = offset - grid_before(row);
    for (const pugi::xml_node cell : children(row, ns::kWord, "tc")) {
        if (remaining < 0) {
            break;
        }
        if (remaining == 0) {
            return cell;
        }
        remaining -= grid_span(cell);
    }
    throw std::runtime_error("no tc element at that grid offset");
}

// The cell a vertically merged cell continues, followed up to the row that holds its content.
pugi::xml_node merge_root(pugi::xml_node row, pugi::xml_node cell) {
    while (continues_merge(cell)) {
        std::int64_t offset = grid_before(row);
        for (pugi::xml_node previous = cell.previous_sibling(); previous;
             previous = previous.previous_sibling()) {
            if (is(previous, ns::kWord, "tc")) {
                offset += grid_span(previous);
            }
        }
        row = row_above(row);
        cell = cell_at_grid_offset(row, offset);
    }
    return cell;
}

// _Row.cells as text: a cell once per grid column it spans.
std::vector<std::string> row_cells(const pugi::xml_node& row) {
    std::vector<std::string> cells;
    for (const pugi::xml_node cell : children(row, ns::kWord, "tc")) {
        const pugi::xml_node root = merge_root(row, cell);
        const std::string text = cell_text(root);
        for (std::int64_t copy = 0; copy < grid_span(root); ++copy) {
            cells.push_back(text);
        }
    }
    return cells;
}

// ---- XLSX values ----

struct Epoch {
    int year;
    unsigned month;
    unsigned day;
};
constexpr Epoch kWindowsEpoch{1899, 12, 30};
constexpr Epoch kMacEpoch{1904, 1, 1};

constexpr std::int64_t kMicrosPerSecond = 1'000'000;
constexpr std::int64_t kMicrosPerDay = 86'400 * kMicrosPerSecond;

std::string clock_text(std::int64_t micros_of_day) {
    const std::int64_t seconds = micros_of_day / kMicrosPerSecond;
    const std::int64_t micros = micros_of_day % kMicrosPerSecond;
    std::string text =
        std::format("{:02}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    if (micros != 0) {
        text += std::format(".{:06}", micros);
    }
    return text;
}

// str() of a timedelta of `days` and `rest` microseconds (0 <= rest < a day).
std::string timedelta_text(std::int64_t days, std::int64_t rest) {
    const std::int64_t seconds = rest / kMicrosPerSecond;
    const std::int64_t micros = rest % kMicrosPerSecond;
    std::string text =
        std::format("{}:{:02}:{:02}", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    if (days != 0) {
        text = std::format("{} day{}, ", days, days == 1 || days == -1 ? "" : "s") + text;
    }
    if (micros != 0) {
        text += std::format(".{:06}", micros);
    }
    return text;
}

// str(datetime.timedelta(microseconds=total)).
std::string timedelta_text(std::int64_t total) {
    std::int64_t days = total / kMicrosPerDay;
    std::int64_t rest = total % kMicrosPerDay;
    if (rest < 0) {
        rest += kMicrosPerDay;
        --days;
    }
    return timedelta_text(days, rest);
}

// The digits of `text` as a number, nullopt when it has more than `max_digits` significant ones.
std::optional<std::int64_t> bounded_digits(std::string_view text, std::size_t max_digits) {
    const std::size_t first = text.find_first_not_of('0');
    if (first == std::string_view::npos) {
        return 0;
    }
    text.remove_prefix(first);
    if (text.size() > max_digits) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    for (const char digit : text) {
        value = value * 10 + (digit - '0');
    }
    return value;
}

// openpyxl's from_ISO8601 (utils/datetime.py) for a cell of type "d", as str() prints the date,
// time, datetime or timedelta it gives. Throws std::runtime_error where Python raises ValueError
// or OverflowError, which ends the workbook's read as openpyxl's does.
std::string iso8601_text(const std::string& value) {
    // ISO_REGEX and ISO_DURATION, matched at the start of the text as re.match does.
    static const std::regex iso(R"((?:(\d{4})-(\d{2})-(\d{2}))?T?)"
                                R"((?:(\d{2}):(\d{2})(?::(\d{2})(\.\d{1,3})?)?)?Z?)");
    static const std::regex duration(R"(PT(?:(\d+)H)?(?:(\d+)M)?(?:(\d+)(?:\.(\d{1,3}))?S)?)");
    const auto invalid = [&] {
        return std::runtime_error("Invalid datetime value " + value);
    };
    std::smatch match;
    if (std::regex_search(value, match, iso, std::regex_constants::match_continuous) &&
        (match[1].matched || match[4].matched)) {
        const auto number = [&](int group) {
            return match[group].matched ? std::stoi(match[group].str()) : 0;
        };
        std::string date;
        if (match[1].matched) {
            const std::chrono::year_month_day ymd{std::chrono::year(number(1)),
                                                  std::chrono::month(number(2)),
                                                  std::chrono::day(number(3))};
            if (number(1) < 1 || !ymd.ok()) {
                throw invalid();
            }
            date = std::format("{:04}-{:02}-{:02}", number(1), number(2), number(3));
            if (!match[4].matched) {
                return date;
            }
        }
        const int hour = number(4);
        const int minute = number(5);
        const int second = number(6);
        if (hour > 23 || minute > 59 || second > 59) {
            throw invalid();
        }
        // int(float(".5") * 1_000_000), truncated.
        const std::int64_t micros =
            match[7].matched ? static_cast<std::int64_t>(std::stod(match[7].str()) * 1'000'000.0)
                             : 0;
        const std::string clock =
            clock_text(((hour * 3600LL) + minute * 60LL + second) * kMicrosPerSecond + micros);
        return date.empty() ? clock : date + " " + clock;
    }
    if (std::regex_search(value, match, duration, std::regex_constants::match_continuous) &&
        (match[1].matched || match[2].matched || match[3].matched)) {
        // timedelta(hours=, minutes=, seconds=): past 999999999 days is OverflowError, which
        // bounds every field well inside int64.
        constexpr std::int64_t kMaxDays = 999'999'999;
        const auto hours = bounded_digits(match[1].str(), 13);
        const auto minutes = bounded_digits(match[2].str(), 15);
        const auto seconds = bounded_digits(match[3].str(), 17);
        if (!hours || !minutes || !seconds) {
            throw std::runtime_error("days must be in -999999999..999999999");
        }
        std::int64_t micros = 0;
        if (match[4].matched) {
            std::string fraction = match[4].str();
            fraction.resize(6, '0');
            micros = std::stoll(fraction);
        }
        const std::int64_t total = *hours * 3600 + *minutes * 60 + *seconds;
        const std::int64_t days = total / 86'400;
        if (days > kMaxDays) {
            throw std::runtime_error("days must be in -999999999..999999999");
        }
        return timedelta_text(days, (total % 86'400) * kMicrosPerSecond + micros);
    }
    throw invalid();
}

// openpyxl's from_excel, as str() prints its result; nullopt where Python raises OverflowError or
// ValueError (the cell then reads "#VALUE!").
std::optional<std::string> excel_date_text(double value, const Epoch& epoch, bool timedelta) {
    if (!std::isfinite(value)) {
        return std::nullopt;
    }
    if (timedelta) {
        const double micros = std::nearbyint(value * static_cast<double>(kMicrosPerDay));
        if (std::abs(micros) > 9.0e17) {
            return std::nullopt;
        }
        auto total = static_cast<std::int64_t>(micros);
        std::int64_t sub = total % kMicrosPerSecond;
        if (sub < 0) {
            sub += kMicrosPerSecond;
        }
        if (sub != 0) {
            // Rounded to milliseconds, half to even, as round(microseconds, -3).
            const std::int64_t whole = total - sub;
            std::int64_t millis = sub / 1000;
            const std::int64_t remainder = sub % 1000;
            if (remainder > 500 || (remainder == 500 && millis % 2 == 1)) {
                ++millis;
            }
            total = whole + millis * 1000;
        }
        return timedelta_text(total);
    }
    double day = std::floor(value);
    const double fraction = value - day;
    const auto millis = static_cast<std::int64_t>(std::nearbyint(fraction * 86'400'000.0));
    const std::int64_t diff = millis * 1000;
    if (value >= 0 && value < 1 && diff < kMicrosPerDay) {
        return clock_text(diff);
    }
    const bool windows = epoch.year == kWindowsEpoch.year;
    if (value > 0 && value < 60 && windows) {
        day += 1;
    }
    if (std::abs(day) > 3.0e6) {
        return std::nullopt;
    }
    using namespace std::chrono;
    const sys_days base =
        sys_days(year_month_day(year(epoch.year), month(epoch.month), std::chrono::day(epoch.day)));
    const std::int64_t total =
        (base.time_since_epoch().count() + static_cast<std::int64_t>(day)) * kMicrosPerDay + diff;
    std::int64_t days_part = total / kMicrosPerDay;
    std::int64_t rest = total % kMicrosPerDay;
    if (rest < 0) {
        rest += kMicrosPerDay;
        --days_part;
    }
    const year_month_day date{sys_days(std::chrono::days(days_part))};
    const int y = static_cast<int>(date.year());
    if (y < 1 || y > 9999) {
        return std::nullopt;
    }
    return std::format("{:04}-{:02}-{:02} ", y, static_cast<unsigned>(date.month()),
                       static_cast<unsigned>(date.day())) +
           clock_text(rest);
}

bool is_date_format(std::string_view format) {
    static const py::Regex strip(R"(".*?"|\[(?!hh?\]|mm?\]|ss?\])[^\]]*\])");
    static const py::Regex letters(R"((?<![_\\])[dmhysDMHYS])");
    const std::string first(format.substr(0, format.find(';')));
    return letters.search(strip.sub("", first)).has_value();
}

bool is_timedelta_format(std::string_view format) {
    static const py::Regex timedelta(
        R"(\[hh?\](:mm(:ss(\.0*)?)?)?|\[mm?\](:ss(\.0*)?)?|\[ss?\](\.0*)?)", py::IGNORECASE);
    const std::string first(format.substr(0, format.find(';')));
    return timedelta.search(first).has_value();
}

// BUILTIN_FORMATS: the built-in number formats whose codes name a date or time part.
std::optional<std::string_view> builtin_format(std::int64_t id) {
    static const std::map<std::int64_t, std::string_view> formats = {
        {14, "mm-dd-yy"},    {15, "d-mmm-yy"},      {16, "d-mmm"},     {17, "mmm-yy"},
        {18, "h:mm AM/PM"},  {19, "h:mm:ss AM/PM"}, {20, "h:mm"},      {21, "h:mm:ss"},
        {22, "m/d/yy h:mm"}, {45, "mm:ss"},         {46, "[h]:mm:ss"}, {47, "mmss.0"},
    };
    const auto found = formats.find(id);
    if (found == formats.end()) {
        return std::nullopt;
    }
    return found->second;
}

struct Styles {
    std::set<std::int64_t> dates;
    std::set<std::int64_t> timedeltas;
};

Styles read_styles(Package& package) {
    Styles styles;
    // apply_stylesheet reads this fixed name, whatever the relationships say.
    if (!package.has("xl/styles.xml")) {
        return styles;
    }
    const pugi::xml_document document = package.xml("xl/styles.xml");
    const pugi::xml_node sheet = child(document, ns::kSheet, "styleSheet");
    std::map<std::int64_t, std::string> custom;
    for (const pugi::xml_node format :
         children(child(sheet, ns::kSheet, "numFmts"), ns::kSheet, "numFmt")) {
        custom[parse_int(format.attribute("numFmtId").value())] =
            format.attribute("formatCode").value();
    }
    std::int64_t index = 0;
    for (const pugi::xml_node xf :
         children(child(sheet, ns::kSheet, "cellXfs"), ns::kSheet, "xf")) {
        const pugi::xml_attribute id_attribute = xf.attribute("numFmtId");
        const std::int64_t id = id_attribute ? parse_int(id_attribute.value()) : 0;
        std::optional<std::string> format;
        if (const auto found = custom.find(id); found != custom.end()) {
            format = found->second;
        } else if (const auto builtin = builtin_format(id)) {
            format = std::string(*builtin);
        }
        if (format && is_date_format(*format)) {
            styles.dates.insert(index);
        }
        if (format && is_timedelta_format(*format)) {
            styles.timedeltas.insert(index);
        }
        ++index;
    }
    return styles;
}

// Text.from_tree(node).content: the plain t, then each rich run's t; phonetic runs are left out.
std::string rich_text(const pugi::xml_node& node) {
    std::string text;
    if (const pugi::xml_node plain = child(node, ns::kSheet, "t")) {
        text += element_text(plain);
    }
    for (const pugi::xml_node run : children(node, ns::kSheet, "r")) {
        if (const pugi::xml_node t = child(run, ns::kSheet, "t")) {
            text += element_text(t);
        }
    }
    return text;
}

std::vector<std::string> read_shared_strings(Package& package) {
    std::vector<std::string> strings;
    const std::string part = package.find_override(content::kSharedStrings);
    if (part.empty()) {
        return strings;
    }
    const pugi::xml_document document = package.xml(std::string_view(part).substr(1));
    for (const pugi::xml_node item : child(document, ns::kSheet, "sst").children()) {
        if (!is(item, ns::kSheet, "si")) {
            continue;
        }
        std::string text = rich_text(item);
        std::string cleaned;
        for (std::size_t at = 0; at < text.size();) {
            if (text.compare(at, 6, "x005F_") == 0) {
                at += 6;
            } else {
                cleaned += text[at++];
            }
        }
        strings.push_back(std::move(cleaned));
    }
    return strings;
}

// coordinate_to_tuple: (row, column) of "B12"; a "$" may precede each half.
std::pair<std::int64_t, std::int64_t> coordinate(std::string_view text) {
    static const py::Regex pattern(R"(^[$]?([A-Za-z]{1,3})[$]?(\d+)$)");
    const auto match = pattern.match(text);
    if (!match) {
        throw std::runtime_error("an invalid cell coordinate");
    }
    std::int64_t column = 0;
    for (const char c : py::upper(match->group(1))) {
        column = column * 26 + (c - 'A' + 1);
    }
    return {parse_int(match->group(2)), column};
}

struct Dimensions {
    std::optional<std::int64_t> max_column;
    std::optional<std::int64_t> max_row;
};

// range_boundaries of the sheet's dimension ref, read before sheetData as openpyxl does.
Dimensions read_dimensions(const pugi::xml_node& worksheet) {
    Dimensions dimensions;
    for (const pugi::xml_node item : worksheet.children()) {
        if (is(item, ns::kSheet, "sheetData")) {
            break;
        }
        if (!is(item, ns::kSheet, "dimension")) {
            continue;
        }
        static const py::Regex range(
            R"(^[$]?(?:([A-Za-z]{1,3})?)[$]?(\d+)?(?::[$]?([A-Za-z]{1,3})?[$]?(\d+)?)?$)");
        const std::string ref = item.attribute("ref").value();
        const auto match = range.match(ref);
        if (!match || ref.empty()) {
            throw std::runtime_error("an invalid sheet dimension");
        }
        const bool ranged = ref.find(':') != std::string::npos;
        const std::string column = ranged ? match->group(3) : match->group(1);
        const std::string row = ranged ? match->group(4) : match->group(2);
        if (!column.empty()) {
            dimensions.max_column = coordinate(column + "1").second;
        }
        if (!row.empty()) {
            dimensions.max_row = parse_int(row);
        }
        break;
    }
    return dimensions;
}

// str() of a "n" value as _cast_number gives it.
std::string number_text(const std::string& value) {
    if (value.find_first_of(".Ee") != std::string::npos) {
        const auto parsed = py::py_float(value);
        if (!parsed) {
            throw std::runtime_error("a cell number is not a float");
        }
        return py::float_repr(*parsed);
    }
    const auto parsed = py::py_int_checked(value);
    if (parsed) {
        return std::to_string(*parsed);
    }
    if (parsed.error() != py::IntError::Overflow) {
        throw std::runtime_error("a cell number is not an integer");
    }
    // Beyond int64: Python's int prints the digits without sign noise or leading zeros.
    std::string digits = py::strip(value);
    bool negative = false;
    if (!digits.empty() && (digits[0] == '+' || digits[0] == '-')) {
        negative = digits[0] == '-';
        digits.erase(0, 1);
    }
    std::erase(digits, '_');
    const auto first = digits.find_first_not_of('0');
    digits = first == std::string::npos ? "0" : digits.substr(first);
    return negative ? "-" + digits : digits;
}

std::optional<std::string> cell_value(const pugi::xml_node& cell,
                                      const std::vector<std::string>& shared, const Styles& styles,
                                      const Epoch& epoch) {
    std::string type = cell.attribute("t") ? cell.attribute("t").value() : "n";
    const pugi::xml_attribute style_attribute = cell.attribute("s");
    const std::int64_t style =
        style_attribute && *style_attribute.value() ? parse_int(style_attribute.value()) : 0;
    if (type == "inlineStr") {
        if (const pugi::xml_node inline_string = child(cell, ns::kSheet, "is")) {
            return rich_text(inline_string);
        }
        return std::nullopt;
    }
    const pugi::xml_node value_node = child(cell, ns::kSheet, "v");
    if (!value_node) {
        return std::nullopt;
    }
    std::string value = element_text(value_node);
    if (value.empty()) {
        return std::nullopt;
    }
    if (type == "n") {
        const std::string text = number_text(value);
        if (styles.dates.contains(style)) {
            const auto number = py::py_float(text);
            return excel_date_text(number.value_or(0.0), epoch, styles.timedeltas.contains(style))
                .value_or("#VALUE!");
        }
        return text;
    }
    if (type == "s") {
        const std::int64_t index = parse_int(value);
        if (index < 0 || index >= static_cast<std::int64_t>(shared.size())) {
            throw std::runtime_error("a shared string index is out of range");
        }
        return shared[static_cast<std::size_t>(index)];
    }
    if (type == "b") {
        return parse_int(value) != 0 ? "True" : "False";
    }
    if (type == "d") {
        // ISO 8601, which Excel writes only in strict files.
        return iso8601_text(value);
    }
    // "str" and "e" keep their text.
    return value;
}

void append_sheet(Package& package, const std::string& part, const std::string& title,
                  const std::vector<std::string>& shared, const Styles& styles, const Epoch& epoch,
                  std::size_t max_rows, std::size_t max_cols, std::vector<std::string>& parts) {
    parts.push_back(title.empty() ? "Sheet" : title);
    const pugi::xml_document document = package.xml(part);
    const pugi::xml_node worksheet = child(document, ns::kSheet, "worksheet");
    const Dimensions dimensions = read_dimensions(worksheet);
    std::size_t yielded = 0;
    std::int64_t counter = 1;
    std::int64_t row_number = 0;
    for (const pugi::xml_node row :
         children(child(worksheet, ns::kSheet, "sheetData"), ns::kSheet, "row")) {
        if (const pugi::xml_attribute r = row.attribute("r")) {
            const auto integer = py::py_int(std::string(r.value()));
            if (integer) {
                row_number = *integer;
            } else {
                const auto real = py::py_float(std::string(r.value()));
                if (!real || std::floor(*real) != *real) {
                    throw std::runtime_error("an invalid row number");
                }
                row_number = static_cast<std::int64_t>(*real);
            }
        } else {
            ++row_number;
        }
        if (dimensions.max_row && row_number > *dimensions.max_row) {
            break;
        }
        // Rows missing from the sheet are empty rows, and count.
        while (counter < row_number) {
            ++counter;
            if (++yielded > max_rows) {
                return;
            }
        }
        if (counter > row_number) {
            continue;
        }
        ++counter;
        if (++yielded > max_rows) {
            return;
        }
        std::map<std::int64_t, std::optional<std::string>> values;
        std::int64_t column_counter = 0;
        std::int64_t last_column = 0;
        for (const pugi::xml_node cell : row.children()) {
            if (cell.type() != pugi::node_element) {
                continue;
            }
            std::int64_t column = 0;
            if (const pugi::xml_attribute reference = cell.attribute("r")) {
                column = coordinate(reference.value()).second;
                column_counter = column;
            } else {
                column = ++column_counter;
            }
            last_column = column;
            values[column] = cell_value(cell, shared, styles, epoch);
        }
        if (values.empty() && !dimensions.max_column) {
            continue;
        }
        const std::int64_t width = dimensions.max_column.value_or(last_column);
        std::vector<std::string> cells;
        for (std::int64_t column = 1;
             column <= std::min<std::int64_t>(width, static_cast<std::int64_t>(max_cols));
             ++column) {
            const auto found = values.find(column);
            if (found == values.end() || !found->second) {
                continue;
            }
            std::string text = py::strip(*found->second);
            if (!text.empty()) {
                cells.push_back(std::move(text));
            }
        }
        if (!cells.empty()) {
            parts.push_back(join_lines(cells, " | "));
        }
    }
}

} // namespace

std::string extract_docx(const fs::path& path) {
    Package package(path);
    const std::string main = package.main_part();
    if (package.content_type(main) != content::kWordMain) {
        throw std::runtime_error("the file is not a Word document");
    }
    const pugi::xml_document document = package.xml(main);
    const pugi::xml_node body = child(child(document, ns::kWord, "document"), ns::kWord, "body");
    std::vector<std::string> parts;
    for (const pugi::xml_node paragraph : children(body, ns::kWord, "p")) {
        std::string text = py::strip(paragraph_text(paragraph));
        if (!text.empty()) {
            parts.push_back(std::move(text));
        }
    }
    for (const pugi::xml_node table : children(body, ns::kWord, "tbl")) {
        for (const pugi::xml_node row : children(table, ns::kWord, "tr")) {
            std::vector<std::string> cells;
            for (const auto& cell : row_cells(row)) {
                std::string text = py::strip(cell);
                if (!text.empty()) {
                    cells.push_back(std::move(text));
                }
            }
            if (!cells.empty()) {
                parts.push_back(join_lines(cells, " | "));
            }
        }
    }
    return join_lines(parts, "\n");
}

std::string extract_pptx(const fs::path& path) {
    Package package(path);
    const std::string main = package.main_part();
    const std::string type = package.content_type(main);
    if (std::find(content::kPresentationMains.begin(), content::kPresentationMains.end(), type) ==
        content::kPresentationMains.end()) {
        throw std::runtime_error("the file is not a PowerPoint presentation");
    }
    std::map<std::string, std::string, std::less<>> targets;
    for (const auto& relationship : package.relationships(main)) {
        targets[relationship.id] = relationship.target;
    }
    const pugi::xml_document presentation = package.xml(main);
    const pugi::xml_node slide_list = child(child(presentation, ns::kPresentation, "presentation"),
                                            ns::kPresentation, "sldIdLst");
    std::vector<std::string> parts;
    for (const pugi::xml_node slide_id : children(slide_list, ns::kPresentation, "sldId")) {
        const pugi::xml_attribute id = attribute(slide_id, ns::kOfficeRels, "id");
        const auto target = targets.find(id.value());
        if (target == targets.end()) {
            throw std::runtime_error("a slide has no relationship");
        }
        const pugi::xml_document slide = package.xml(target->second);
        const pugi::xml_node tree =
            child(child(child(slide, ns::kPresentation, "sld"), ns::kPresentation, "cSld"),
                  ns::kPresentation, "spTree");
        for (const pugi::xml_node shape : children(tree, ns::kPresentation, "sp")) {
            const pugi::xml_node body = child(shape, ns::kPresentation, "txBody");
            std::vector<std::string> paragraphs;
            for (const pugi::xml_node paragraph : children(body, ns::kDrawing, "p")) {
                std::string text;
                for (const pugi::xml_node item : paragraph.children()) {
                    if (is(item, ns::kDrawing, "r") || is(item, ns::kDrawing, "fld")) {
                        if (const pugi::xml_node t = child(item, ns::kDrawing, "t")) {
                            text += element_text(t);
                        }
                    } else if (is(item, ns::kDrawing, "br")) {
                        text += '\v';
                    }
                }
                paragraphs.push_back(std::move(text));
            }
            std::string text = py::strip(join_lines(paragraphs, "\n"));
            if (!text.empty()) {
                parts.push_back(std::move(text));
            }
        }
    }
    return join_lines(parts, "\n");
}

std::string extract_xlsx(const fs::path& path, std::size_t max_rows, std::size_t max_cols) {
    Package package(path);
    std::string workbook_part;
    for (const auto type : content::kWorkbookMains) {
        const std::string found = package.find_override(type);
        if (!found.empty()) {
            workbook_part = found.substr(1);
            break;
        }
    }
    if (workbook_part.empty()) {
        // Some applications declare the workbook through the default of application/xml.
        const bool declared =
            std::any_of(content::kWorkbookMains.begin(), content::kWorkbookMains.end(),
                        [&](std::string_view type) { return package.default_declares(type); });
        if (!declared) {
            throw std::runtime_error("the file contains no valid workbook part");
        }
        workbook_part = "xl/workbook.xml";
    }
    const std::vector<std::string> shared = read_shared_strings(package);
    const Styles styles = read_styles(package);
    std::map<std::string, Relationship, std::less<>> relationships;
    for (auto& relationship : package.relationships(workbook_part)) {
        relationships[relationship.id] = relationship;
    }
    const pugi::xml_document workbook = package.xml(workbook_part);
    const pugi::xml_node root = child(workbook, ns::kSheet, "workbook");
    Epoch epoch = kWindowsEpoch;
    if (const pugi::xml_node properties = child(root, ns::kSheet, "workbookPr")) {
        const std::string date1904 = py::lower(properties.attribute("date1904").value());
        if (date1904 == "1" || date1904 == "true") {
            epoch = kMacEpoch;
        }
    }
    std::vector<std::string> parts;
    for (const pugi::xml_node sheet :
         children(child(root, ns::kSheet, "sheets"), ns::kSheet, "sheet")) {
        const pugi::xml_attribute id = attribute(sheet, ns::kOfficeRels, "id");
        if (!id || *id.value() == 0) {
            continue;
        }
        const auto found = relationships.find(id.value());
        if (found == relationships.end()) {
            throw std::runtime_error("a sheet has no relationship");
        }
        const Relationship& relationship = found->second;
        if (!package.has(relationship.target) ||
            relationship.type.find("chartsheet") != std::string::npos) {
            continue;
        }
        append_sheet(package, relationship.target, sheet.attribute("name").value(), shared, styles,
                     epoch, max_rows, max_cols, parts);
    }
    return join_lines(parts, "\n");
}

} // namespace fv::meeting_intel::detail
