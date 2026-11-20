#include "agentapi/federation.h"

#include <algorithm>
#include <future>
#include <utility>

#include "agentapi/history_store.h"
#include "base/hash.h"
#include "base/ids.h"
#include "pyport/json.h"

namespace fv::agentapi {
namespace {

using namespace std::chrono_literals;

const Json& member(const Json& object, std::string_view key) {
    static const Json empty;
    const auto at = object.find(std::string(key));
    return at == object.end() ? empty : *at;
}

std::string text(const Json& object, std::string_view name) {
    const auto& value = member(object, name);
    return value.is_string() ? value.get<std::string>() : std::string();
}

bool has_id(const Json& item, std::string_view key) {
    const auto id = text(item, key);
    return !id.empty() && id.size() <= 128 && id.find_first_of("/\\?#") == std::string::npos &&
           std::ranges::none_of(id, [](unsigned char c) { return c < 32 || c == 127; });
}

std::string quote(std::string_view input) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string result;
    for (const auto character : input) {
        const auto ch = static_cast<unsigned char>(character);
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            result += character;
        } else {
            result += '%';
            result += digits[ch >> 4];
            result += digits[ch & 15];
        }
    }
    return result;
}

Json annotate(const Json& item, std::string_view operation, const ClientStatus& peer) {
    if (!item.is_object() || !has_id(item, operation == "insights" ? "meeting_id" : "id")) {
        throw remote::ClientUnavailable();
    }
    Json result = Json::object();
    // Only response fields the public API knows can leave the peer.
    for (const auto field : {
             "id",
             "title",
             "timestamp",
             "model",
             "source_name",
             "audio_duration",
             "preview",
             "text",
             "raw_text",
             "cleanup_provider",
             "cleanup_model",
             "started_at",
             "ended_at",
             "status",
             "meeting_id",
             "kind",
             "excerpt",
             "matched_field",
             "start_s",
             "end_s",
             "speaker",
             "speaker_id",
             "speaker_name",
             "source",
             "language",
             "duration_s",
             "segment_count",
             "participant_count",
             "state_seq",
             "snapshot_available",
             "rolling_summary",
             "rolling_summary_evidence",
             "participants",
             "cards",
             "questions",
             "custom_reports",
         }) {
        if (item.contains(field)) {
            result[field] = item[field];
        }
    }
    result["device_id"] = peer.device_id;
    if (operation != "segment" && operation != "segments" && operation != "insights") {
        result["origin_device_id"] = peer.device_id;
        result["origin_device_name"] = peer.name;
    }
    if (operation == "search") {
        const auto kind = text(item, "kind");
        if (kind != "transcription" && kind != "meeting" && kind != "segment") {
            throw remote::ClientUnavailable();
        }
        const auto id = quote(text(item, "id"));
        std::string resource =
            kind == "transcription" ? "/v1/transcriptions/" + id : "/v1/meetings/" + id;
        if (kind == "segment") {
            if (!has_id(item, "meeting_id")) {
                throw remote::ClientUnavailable();
            }
            resource = "/v1/meetings/" + quote(text(item, "meeting_id")) + "/segments/" + id;
        }
        result["resource"] =
            resource + "?device_id=" + quote(peer.device_id) + "&include_remote=true";
    }
    return result;
}

std::string time_of(const Json& item) {
    auto result = text(item, "timestamp");
    return result.empty() ? text(item, "started_at") : result;
}

bool earlier(const Json& a, const Json& b) {
    const auto left = time_of(a);
    const auto right = time_of(b);
    if (left != right) {
        return left > right;
    }
    return std::tuple(text(a, "kind"), text(a, "id"), text(a, "origin_device_id")) <
           std::tuple(text(b, "kind"), text(b, "id"), text(b, "origin_device_id"));
}

} // namespace

FederatedHistory::FederatedHistory(std::shared_ptr<remote::ClientHistorySource> clients,
                                   const Clock& clock)
    : clients_(std::move(clients)), clock_(clock) {}

std::vector<ClientStatus> FederatedHistory::clients() {
    std::vector<ClientStatus> peers;
    for (auto client : clients_->clients()) {
        peers.push_back(
            {std::move(client.device_id), std::move(client.name), std::move(client.status)});
    }
    return peers;
}

Json FederatedHistory::query(std::string_view device_id, std::string_view op, const Json& params) {
    if (op != "transcription" && op != "transcriptions" && op != "meeting" && op != "meetings" &&
        op != "search" && op != "segment" && op != "segments" && op != "insights") {
        throw remote::ClientUnavailable("invalid_query");
    }
    const auto peers = clients();
    const auto peer = std::ranges::find_if(
        peers, [device_id](const ClientStatus& status) { return status.device_id == device_id; });
    if (peer == peers.end() || peer->status != "online") {
        throw remote::ClientUnavailable(peer != peers.end() && peer->status == "sharing_disabled"
                                            ? "sharing_disabled"
                                            : "unavailable");
    }
    const auto result = clients_->query(device_id, op, params);
    if (op != "transcriptions" && op != "meetings" && op != "search" && op != "segments") {
        return annotate(result, op, *peer);
    }
    const auto& rows = member(result, "items");
    const auto limit = params.value("limit", 20);
    if (!rows.is_array() || rows.size() > static_cast<std::size_t>(limit)) {
        throw remote::ClientUnavailable();
    }
    const auto next = text(result, "next_cursor");
    if (next.size() > 2048) {
        throw remote::ClientUnavailable();
    }
    Json safe = Json::array();
    for (const auto& item : rows) {
        safe.push_back(annotate(item, op, *peer));
    }
    return page_json(std::move(safe), next.empty() ? std::nullopt : std::make_optional(next));
}

std::string FederatedHistory::save(Page page) {
    const auto id = "clients-" + uuid4_hex();
    std::lock_guard lock(mutex_);
    const auto now = clock_.steady();
    std::erase_if(cursors_, [now](const auto& item) { return item.second.expires < now; });
    if (cursors_.size() >= 128) {
        const auto oldest = std::ranges::min_element(
            cursors_, {}, [](const auto& item) { return item.second.sequence; });
        cursors_.erase(oldest);
    }
    page.expires = now + 600s;
    page.sequence = sequence_++;
    cursors_.emplace(id, std::move(page));
    return id;
}

Json FederatedHistory::page(std::string_view operation, const Json& params,
                            const std::optional<std::string>& device_id, bool include_remote,
                            const std::optional<std::string>& cursor, std::int64_t limit,
                            const LocalRead& read) {
    if (limit < 1 || limit > 100) {
        throw InvalidQuery("limit must be between 1 and 100.");
    }
    py::DumpOptions sorted;
    sorted.sort_keys = true;
    const auto scope = sha256_hex(py::dumps(Json::array({
                                                std::string(operation),
                                                params,
                                                device_id ? Json(*device_id) : Json(),
                                                limit,
                                            }),
                                            sorted));
    auto statuses = clients();
    if (device_id) {
        std::erase_if(statuses,
                      [&](const ClientStatus& status) { return status.device_id != *device_id; });
        if (statuses.empty()) {
            if (!include_remote) {
                throw remote::ClientUnavailable();
            }
            statuses.push_back({*device_id, "Paired computer", "unavailable"});
        }
    }
    Page page;
    page.scope = scope;
    if (cursor && !cursor->empty()) {
        std::lock_guard lock(mutex_);
        const auto at = cursors_.find(*cursor);
        if (at == cursors_.end() || at->second.expires < clock_.steady() ||
            at->second.scope != scope) {
            throw InvalidQuery("Client history cursor expired or used with different filters. "
                               "Restart the query.");
        }
        page = at->second;
    } else {
        if (!device_id || include_remote) {
            page.sources.push_back({});
        }
        for (const auto& peer : statuses) {
            page.sources.push_back(
                {.device = peer.device_id, .exhausted = peer.status != "online"});
        }
    }

    using Pending = std::pair<std::size_t, std::future<Json>>;
    const auto status_for = [&](std::string_view id) {
        return std::ranges::find_if(
            statuses, [id](const ClientStatus& status) { return status.device_id == id; });
    };
    const auto load = [&](const Source& source) {
        if (!source.device) {
            return read(source.cursor, limit, device_id);
        }
        Json query_params = params;
        query_params.erase("include_remote");
        query_params["limit"] = limit;
        query_params["cursor"] = source.cursor ? Json(*source.cursor) : Json();
        return query(*source.device, operation, query_params);
    };
    const auto finish = [&](Source& source, std::future<Json>& result) {
        Json response;
        try {
            response = result.get();
        } catch (const remote::ClientUnavailable& error) {
            source.exhausted = true;
            if (source.device) {
                const auto peer = status_for(*source.device);
                if (peer != statuses.end()) {
                    peer->status =
                        error.code() == "sharing_disabled" ? "sharing_disabled" : "unavailable";
                }
            }
            return;
        }
        auto& rows = response["items"];
        source.items.assign(std::make_move_iterator(rows.begin()),
                            std::make_move_iterator(rows.end()));
        const auto next = text(response, "next_cursor");
        source.exhausted = next.empty() || (source.cursor && next == *source.cursor);
        source.cursor = source.exhausted ? std::nullopt : std::make_optional(next);
    };

    std::vector<std::size_t> reads(page.sources.size(), 0);
    std::vector<Pending> pending;
    const auto drain = [&] {
        for (auto& [i, future] : pending) {
            finish(page.sources[i], future);
        }
        pending.clear();
    };
    for (std::size_t i = 0; i < page.sources.size(); ++i) {
        auto& source = page.sources[i];
        if (source.device) {
            const auto peer = status_for(*source.device);
            if (peer == statuses.end() || peer->status != "online") {
                source.items.clear();
                source.exhausted = true;
            }
        }
        if (source.items.empty() && !source.exhausted) {
            ++reads[i];
            pending.emplace_back(i, std::async(std::launch::async, load, std::cref(source)));
            if (pending.size() == 8) {
                drain();
            }
        }
    }
    drain();

    Json output = Json::array();
    while (static_cast<std::int64_t>(output.size()) < limit) {
        Source* chosen = nullptr;
        for (std::size_t i = 0; i < page.sources.size(); ++i) {
            auto& source = page.sources[i];
            if (source.items.empty() && !source.exhausted && reads[i] < 8) {
                ++reads[i];
                auto future = std::async(std::launch::deferred, load, std::cref(source));
                finish(source, future);
            }
            if (!source.items.empty() &&
                (!chosen || earlier(source.items.front(), chosen->items.front()))) {
                chosen = &source;
            }
        }
        if (!chosen) {
            break;
        }
        auto item = std::move(chosen->items.front());
        chosen->items.pop_front();
        const Identity identity{text(item, "origin_device_id"), text(item, "kind"),
                                text(item, "id")};
        if (page.seen.insert(identity).second) {
            output.push_back(std::move(item));
        }
    }
    std::optional<std::string> next;
    if (std::ranges::any_of(page.sources, [](const Source& source) {
            return !source.items.empty() || !source.exhausted;
        })) {
        next = save(std::move(page));
    }
    return page_json(std::move(output), next, statuses);
}

} // namespace fv::agentapi
