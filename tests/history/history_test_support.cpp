#include "history_test_support.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>

#include "base/utf8.h"
#include "sql/db.h"

namespace fv::history_test {

TestDatabase::TestDatabase()
    : database_(std::make_unique<Database>(Database::open_or_create(root_.path() / "test.db"))),
      repository_(*database_) {}

TestDatabase::~TestDatabase() {
    database_->close();
}

void TestDatabase::add(std::string id, std::string text, std::string timestamp, std::string model,
                       ContextFields context, std::optional<std::string> source_name,
                       std::optional<std::string> raw_text, std::optional<double> audio_duration) {
    HistoryEntry entry;
    entry.id = std::move(id);
    entry.text = std::move(text);
    entry.timestamp = std::move(timestamp);
    entry.model = std::move(model);
    entry.source_name = std::move(source_name);
    entry.raw_text = std::move(raw_text);
    entry.audio_duration = audio_duration;
    repository_.add_history_entry(entry, context);
}

void TestDatabase::set_origin(std::string_view id, std::string_view device) {
    database_->write([&](sql::Connection& conn) {
        sql::Statement statement =
            conn.prepare("UPDATE transcription_history SET origin_device_id = ? WHERE id = ?");
        statement.bind(1, std::string(device));
        statement.bind(2, std::string(id));
        statement.step();
    });
}

std::vector<std::string> TestDatabase::column(std::string_view query) {
    return database_->read([&](sql::Connection& conn) {
        sql::Statement statement = conn.prepare(query);
        std::vector<std::string> values;
        while (statement.step()) {
            const sql::Value value = statement.column(0);
            if (const auto* text = std::get_if<std::string>(&value)) {
                values.push_back(*text);
            } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                values.push_back(std::to_string(*integer));
            } else {
                values.emplace_back("<null>");
            }
        }
        return values;
    });
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::vector<std::string> list_names(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& item : std::filesystem::directory_iterator(folder)) {
        names.push_back(path_to_utf8(item.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

std::chrono::system_clock::time_point utc(int year, unsigned month, unsigned day, int hour,
                                          int minute, int second) {
    using namespace std::chrono;
    return sys_days{std::chrono::year{year} / month / day} + hours{hour} + minutes{minute} +
           seconds{second};
}

} // namespace fv::history_test
