#include "remote_host/dispatch.h"

#include <chrono>
#include <exception>
#include <utility>

#include "base/identity.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_host/detail/pyvalue.h"

namespace fv::remote_host {

namespace {

constexpr log::Logger logger("services.remote_asr.host");

using detail::get;
using detail::has_other_keys;
using detail::is_text;
using detail::nonempty_string;

Json error_reply(const Json& id, std::string error) {
    Json reply = Json::object();
    reply["id"] = id;
    reply["error"] = std::move(error);
    return reply;
}

// {"id", "code", "error"}: the key order of the replies Python writes with the code first.
Json coded_reply(const Json& id, std::string_view code, std::string error) {
    Json reply = Json::object();
    reply["id"] = id;
    reply["code"] = std::string(code);
    reply["error"] = std::move(error);
    return reply;
}

Json result_reply(const Json& id, Json result) {
    Json reply = Json::object();
    reply["id"] = id;
    reply["result"] = std::move(result);
    return reply;
}

bool is_device(const Json& value) {
    return is_text(value, "auto") || is_text(value, "cpu") || is_text(value, "cuda");
}

// The probe's subset of describe(): label, device, available, family (null when missing).
Json engine_subset(const Json& described) {
    Json out = Json::object();
    for (const char* key : {"label", "device", "available", "family"}) {
        out[key] = get(described, key);
    }
    return out;
}

double seconds_since(Clock& clock, std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double>(clock.steady() - started).count();
}

} // namespace

std::string host_update_message() {
    const std::string_view product = identity().product;
    const bool vowel = !product.empty() && std::string_view("AEIOUaeiou").find(product.front()) !=
                                               std::string_view::npos;
    return std::string("The host is preparing ") + (vowel ? "an " : "a ") + std::string(product) +
           " update. Try again shortly.";
}

bool is_costly(const Json& op) {
    for (const char* name : {"transcribe", "stream", "select_model", "configure_runtime",
                             "model_catalog", "download_model", "install_runtime"}) {
        if (is_text(op, name)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> header_language(const Json& header) {
    const Json& language = get(header, "language");
    if (language.is_string()) {
        return language.get<std::string>();
    }
    return std::nullopt;
}

Json transcribe_request(HostEngine& engine, const Json& header, std::span<const float> audio) {
    const auto language = header_language(header);
    std::vector<std::string> phrases;
    if (engine.accepts_phrases()) {
        phrases = remote::header_phrases(header);
    }
    return engine.transcribe(audio, language, phrases);
}

Json stream_request(HostEngine& engine, const Json& header, std::span<const float> audio,
                    std::string_view connection_id, std::set<std::string>& streams) {
    const Json& session = get(header, "session");
    if (!nonempty_string(session)) {
        throw std::invalid_argument("A stream needs a session id");
    }
    // Sessions are per connection, so two clients (or the host's own preview) never share one.
    std::string scoped = "remote-";
    scoped += connection_id;
    scoped += "-";
    scoped += session.get<std::string>();
    if (!is_text(get(header, "op"), "stream")) {
        streams.erase(scoped);
        engine.cancel_stream(scoped);
        return Json::object();
    }
    const bool finish = detail::truthy(get(header, "finish"));
    streams.insert(scoped);
    Json result = engine.stream(scoped, audio, header_language(header), finish);
    if (finish) {
        streams.erase(scoped);
    }
    return result;
}

// ---- Dispatcher --------------------------------------------------------------------------------

Dispatcher::Dispatcher(const SpeechHostDeps& deps, HostActivity& activity, Emit emit)
    : deps_(deps), activity_(activity), emit_(std::move(emit)) {}

void Dispatcher::emit(std::string kind, Json info) const {
    if (emit_) {
        emit_(std::move(kind), std::move(info));
    }
}

std::shared_ptr<HostEngine> Dispatcher::engine() const {
    std::shared_ptr<HostEngine> engine;
    try {
        if (deps_.engine_provider) {
            engine = deps_.engine_provider();
        }
    } catch (const std::exception& error) {
        logger.warning("Remote engine provider failed: {}", error.what());
        return unavailable_engine("The host's engine is unavailable");
    }
    if (!engine) {
        return unavailable_engine("No engine is selected on the host");
    }
    return engine;
}

Json Dispatcher::host_info() const {
    Json addresses = Json::array();
    try {
        if (deps_.addresses) {
            for (const auto& address : deps_.addresses()) {
                if (!address.empty()) {
                    addresses.push_back(address);
                }
            }
        }
    } catch (const std::exception&) {
        logger.debug("Remote engine host addresses lookup failed");
        addresses = Json::array();
    }
    Json out = Json::object();
    out["name"] = deps_.host_name;
    out["fingerprint"] = deps_.identity.fingerprint;
    out["addresses"] = std::move(addresses);
    return out;
}

Json Dispatcher::model_list() const {
    Json out = Json::array();
    try {
        if (deps_.models) {
            const Json models = deps_.models();
            if (models.is_array()) {
                for (const auto& entry : models) {
                    if (entry.is_object()) {
                        out.push_back(entry);
                    }
                }
            }
        }
    } catch (const std::exception&) {
        logger.warning("Remote engine host couldn't list its models");
        return Json::array();
    }
    return out;
}

bool Dispatcher::can_manage_models() const {
    try {
        return deps_.manage_models && deps_.model_management && deps_.model_management();
    } catch (const std::exception&) {
        logger.warning("Could not read model management permission");
        return false;
    }
}

bool Dispatcher::keeps_records() const {
    try {
        return deps_.records && deps_.records_enabled && deps_.records_enabled();
    } catch (const std::exception&) {
        logger.warning("Could not read the record storage permission");
        return false;
    }
}

bool Dispatcher::controls_mcp() const {
    try {
        return deps_.mcp && deps_.mcp_enabled && deps_.mcp_enabled();
    } catch (const std::exception&) {
        logger.warning("Could not read the MCP control permission");
        return false;
    }
}

Json Dispatcher::stored_summary(std::string_view device_id) const {
    try {
        if (deps_.records_summary) {
            Json summary = deps_.records_summary(device_id);
            if (summary.is_object()) {
                return summary;
            }
        }
    } catch (const std::exception&) {
        logger.warning("Could not count a device's stored records");
    }
    return Json::object();
}

std::string Dispatcher::tailscale_pairing_owner() const {
    try {
        return deps_.tailscale_owner ? deps_.tailscale_owner() : std::string();
    } catch (const std::exception&) {
        logger.debug("Tailscale owner lookup failed");
        return "";
    }
}

bool Dispatcher::engine_settled() const {
    return !deps_.engine_settled || deps_.engine_settled();
}

Json Dispatcher::runtime() const {
    return deps_.runtime ? deps_.runtime() : Json::object();
}

Json Dispatcher::ready_message(const remote::PairedDevice& device, HostEngine& engine,
                               bool maintenance) const {
    Json ready = Json::object();
    ready["type"] = "ready";
    ready["host"] = host_info();
    Json who = Json::object();
    who["id"] = device.id;
    who["name"] = device.name;
    ready["device"] = std::move(who);
    ready["engine"] = engine.describe();
    ready["models"] = model_list();
    ready["runtime"] = runtime();
    Json capabilities = Json::object();
    const bool manages = can_manage_models();
    capabilities["model_management"] = manages;
    capabilities["runtime_installation"] = manages;
    capabilities["engine_controls"] = static_cast<bool>(deps_.configure_runtime);
    capabilities["recognition_hints"] = engine.accepts_phrases();
    capabilities["recording_activity"] = true;
    ready["capabilities"] = std::move(capabilities);
    if (maintenance || !engine_settled()) {
        Json& described = ready["engine"];
        if (!described.is_object()) {
            described = Json::object();
        }
        described["available"] = false;
        described["status"] =
            maintenance ? host_update_message() : std::string(ENGINE_LOADING_MESSAGE);
    }
    if (deps_.mcp_peer_request && deps_.mcp_peer_allowed) {
        ready["capabilities"]["mcp_control"] = deps_.mcp_peer_allowed(device.id);
    } else if (deps_.mcp) {
        ready["capabilities"]["mcp_control"] = controls_mcp();
    }
    if (deps_.records) {
        ready["capabilities"]["records"] = keeps_records();
        // Counted even while storage is off, so a client can still find (and bring back) what
        // it stored before the owner turned it off.
        ready["records"] = stored_summary(device.id);
    }
    return ready;
}

Json Dispatcher::probe_message() const {
    const Json described = engine()->describe();
    Json out = Json::object();
    out["type"] = "probe";
    out["protocol"] = remote::PROTOCOL_VERSION;
    Json host = Json::object();
    host["name"] = deps_.host_name;
    out["host"] = std::move(host);
    out["engine"] = engine_subset(described);
    out["tailscale_pairing"] = !tailscale_pairing_owner().empty();
    out["approval"] = true;
    return out;
}

Json Dispatcher::discovery_info(std::optional<std::uint16_t> port) const {
    const Json described = engine()->describe();
    Json out = Json::object();
    out["name"] = deps_.host_name;
    out["port"] = port ? Json(*port) : Json(nullptr);
    out["fingerprint"] = deps_.identity.fingerprint;
    out["engine"] = engine_subset(described);
    out["approval"] = true;
    return out;
}

Json Dispatcher::dispatch_unpacked(const Request& request) const {
    const Json& id = get(request.header, "id");
    const Json& op = get(request.header, "op");
    if (op.is_string() && op.get_ref<const std::string&>().starts_with("records_")) {
        remote::PairedDevice device;
        device.id = std::string(request.device_id);
        device.name = std::string(request.device_name);
        return records_request(id, request.header, request.payload, device);
    }
    if (op.is_array() || op.is_object()) {
        // `op in frozenset(...)`: Python cannot hash a list or a dict.
        throw py::TypeError(std::string("unhashable type: '") + (op.is_array() ? "list" : "dict") +
                            "'");
    }
    if (is_text(op, "mcp_state") || is_text(op, "mcp_configure")) {
        return mcp_request(id, request);
    }
    std::vector<float> audio;
    try {
        audio = remote::payload_audio(request.payload);
    } catch (const remote::ProtocolError& error) {
        Json reply = error_reply(id, error.what());
        reply["code"] = "bad_request";
        return reply;
    }
    if (Json managed = management_request(id, request.header, request.device_name);
        !managed.is_null()) {
        return managed;
    }
    if ((is_text(op, "transcribe") || is_text(op, "stream")) && !engine_settled()) {
        return coded_reply(id, remote::ENGINE_RELOADING, std::string(ENGINE_LOADING_MESSAGE));
    }
    const auto current = engine();
    if (current->identity() != request.identity) {
        return coded_reply(id, "engine_changed", "The host switched speech engines. Reconnecting.");
    }
    return decode_request(request, audio, *current);
}

Json Dispatcher::management_request(const Json& id, const Json& header,
                                    std::string_view device_name) const {
    const Json& op = get(header, "op");
    if (is_text(op, "configure_runtime")) {
        return configure_runtime_request(id, header, device_name);
    }
    if (is_text(op, "model_catalog") || is_text(op, "download_model") ||
        is_text(op, "install_runtime")) {
        return manage_model_request(id, header, device_name);
    }
    if (is_text(op, "select_model")) {
        // Asked for whatever this computer runs now, so a switch made since this client
        // connected doesn't turn it away.
        return switch_model(id, header, device_name);
    }
    return nullptr;
}

Json Dispatcher::decode_request(const Request& request, std::span<const float> audio,
                                HostEngine& current) const {
    const Json& id = get(request.header, "id");
    const Json& op = get(request.header, "op");
    const bool transcribes = is_text(op, "transcribe");
    const bool streams = is_text(op, "stream");
    const bool decodes = transcribes || streams;
    if (decodes && request.set_busy) {
        request.set_busy(true);
    }
    struct Unbusy {
        const Request& owner;
        bool active;
        ~Unbusy() {
            if (active && owner.set_busy) {
                owner.set_busy(false);
            }
        }
    } unbusy{request, decodes};
    const auto started = deps_.clock.steady();
    Json result;
    try {
        if (transcribes) {
            result = transcribe_request(current, request.header, audio);
        } else if (streams || is_text(op, "cancel_stream")) {
            result = stream_request(current, request.header, audio, request.connection_id,
                                    request.streams);
        } else if (is_text(op, "describe")) {
            result = current.describe();
        } else {
            throw std::invalid_argument("Unknown operation: " + detail::py_repr(op));
        }
        // Counted before the busy flag clears, so the "activity" event that clearing sends
        // already sees it.
        if (transcribes) {
            activity_.transcribed(request.device_id, request.device_name,
                                  static_cast<double>(audio.size()) / remote::SAMPLE_RATE,
                                  seconds_since(deps_.clock, started));
        } else if (streams) {
            activity_.previewed(request.device_id, request.device_name);
        }
    } catch (const std::exception& error) {
        // Stream previews arrive many times a second: keep their failures out of the log.
        const std::string what = detail::py_str(op);
        if (streams || is_text(op, "cancel_stream")) {
            logger.debug("Remote {} request from {} failed", what, request.device_name);
        } else {
            logger.warning("Remote {} request from {} failed", what, request.device_name);
        }
        std::string text = detail::error_text(error);
        if (decodes) {
            activity_.failed(request.device_id, request.device_name, text);
        }
        return error_reply(id, std::move(text));
    }
    return result_reply(id, std::move(result));
}

Json Dispatcher::manage_model_request(const Json& id, const Json& header,
                                      std::string_view device_name) const {
    // Checked for every request, not just hello: turning the setting off also denies
    // already-connected clients. Model selection does not need this opt-in.
    if (!can_manage_models()) {
        return coded_reply(id, "forbidden",
                           "Model management is disabled on the host. Enable it in Settings "
                           "\u2192 Remote engine there.");
    }
    const Json& op = get(header, "op");
    const bool catalog = is_text(op, "model_catalog");
    const bool install = is_text(op, "install_runtime");
    bool extra = false;
    if (catalog) {
        extra = has_other_keys(header, {"id", "op"});
    } else if (install) {
        extra = has_other_keys(header, {"id", "op", "family", "model", "device"});
    } else {
        extra = has_other_keys(header, {"id", "op", "family", "model"});
    }
    // Which devices the family offers on this host is checked downstream.
    if (extra ||
        (!catalog &&
         !(nonempty_string(get(header, "family")) && nonempty_string(get(header, "model")))) ||
        (install && !is_device(get(header, "device")))) {
        return coded_reply(id, "bad_request", "Invalid model management request.");
    }
    try {
        return result_reply(id, deps_.manage_models(op.get<std::string>(), header, device_name));
    } catch (const std::exception& error) {
        logger.warning("Model request {} from {} failed", op.get<std::string>(), device_name);
        return error_reply(id, detail::error_text(error));
    }
}

Json Dispatcher::mcp_request(const Json& id, const Request& request) const {
    const auto& header = request.header;
    const auto device_name = request.device_name;
    // The permission is read on every request, so turning it off also stops computers that are
    // already connected. The state includes the access token, which is why this is an opt-in
    // and not part of pairing.
    if (!deps_.mcp_peer_request && !deps_.mcp) {
        return coded_reply(id, "unsupported",
                           "Update " + std::string(identity().product) +
                               " on the host to manage its MCP server.");
    }
    const bool allowed = deps_.mcp_peer_request
                             ? deps_.mcp_peer_allowed && deps_.mcp_peer_allowed(request.device_id)
                             : controls_mcp();
    if (!allowed && deps_.mcp_peer_request) {
        // FioriVoce's host grants one paired computer a session (a17-paired-mcp); OpenWhisper's
        // "Allow paired computers to manage MCP" switch is not on its Remote engine page.
        return coded_reply(id, "forbidden",
                           deps_.host_name +
                               " hasn't granted this computer MCP control. Turn on \"Grant "
                               "session MCP control to one paired computer\" in Settings "
                               "\u2192 Remote engine there and choose this computer.");
    }
    if (!allowed) {
        return coded_reply(id, "forbidden",
                           deps_.host_name +
                               " isn't letting paired computers manage its MCP server. Turn on "
                               "\"Allow paired computers to manage MCP\" in Settings "
                               "\u2192 "
                               "Remote engine there.");
    }
    const std::string op = get(header, "op").get<std::string>();
    const bool configure = op == "mcp_configure";
    const bool extra = configure ? has_other_keys(header, {"id", "op", "settings"})
                                 : has_other_keys(header, {"id", "op"});
    if (extra || (configure && !get(header, "settings").is_object())) {
        return coded_reply(id, "bad_request", "Invalid MCP request.");
    }
    Json result;
    try {
        result = deps_.mcp_peer_request
                     ? deps_.mcp_peer_request(op, header, request.device_id, request.connection_id)
                     : deps_.mcp(op, header, device_name);
    } catch (const py::ValueError& error) {
        // ControlError ("code: public text") carries nothing but a safe message.
        const std::string text = error.what();
        const auto colon = text.find(": ");
        if (colon != std::string::npos && colon + 2 < text.size()) {
            return coded_reply(id, text.substr(0, colon), text.substr(colon + 2));
        }
        return coded_reply(id, "bad_request", text);
    } catch (const std::exception& error) {
        logger.warning("MCP request {} from {} failed", op, device_name);
        return error_reply(id, detail::error_text(error));
    }
    if (configure) {
        logger.info("{} changed this computer's MCP settings", device_name);
        Json info = Json::object();
        info["by"] = std::string(device_name);
        emit("mcp", std::move(info));
    }
    return result_reply(id, std::move(result));
}

Json Dispatcher::records_request(const Json& id, const Json& header,
                                 std::span<const std::byte> payload,
                                 const remote::PairedDevice& device) const {
    // Storing needs the owner's opt-in, rechecked on every request, so turning it off stops
    // uploads already under way. Reading back and deleting don't: a computer can always
    // retrieve or remove what it stored here while it stays paired.
    const Json& op = get(header, "op");
    if (!deps_.records) {
        return coded_reply(id, "unsupported",
                           "Update " + std::string(identity().product) +
                               " on the host to keep records there.");
    }
    const bool writes =
        is_text(op, "records_begin") || is_text(op, "records_put") || is_text(op, "records_commit");
    if (writes && !keeps_records()) {
        return coded_reply(id, "forbidden",
                           deps_.host_name +
                               " isn't keeping records for other computers. Turn on \"Keep "
                               "records for paired computers\" in Settings "
                               "\u2192 Remote engine "
                               "there.");
    }
    if (device.id.empty()) {
        return coded_reply(id, "bad_request", "Unknown device.");
    }
    const std::string name = op.is_string() ? op.get<std::string>() : std::string();
    Json result;
    try {
        result = deps_.records(name, header, payload, device);
    } catch (const RecordsBusy& error) {
        Json reply = Json::object();
        reply["id"] = id;
        reply["code"] = "busy";
        reply["retry_after_ms"] = 1000;
        reply["error"] = error.what();
        return reply;
    } catch (const RecordNotFound& error) {
        const std::string text = error.what();
        return coded_reply(id, "not_found", text.empty() ? "No such record." : text);
    } catch (const py::ValueError& error) {
        const std::string text = error.what();
        return coded_reply(id, "bad_request", text.empty() ? "Invalid record request." : text);
    } catch (const std::exception& error) {
        logger.warning("Record request {} from {} failed", name, device.name);
        return error_reply(id, detail::error_text(error));
    }
    if (name == "records_commit" || name == "records_delete" || name == "records_delete_version" ||
        name == "records_clear") {
        Json info = Json::object();
        info["device"] = device.name;
        info["op"] = name;
        emit("records", std::move(info));
    }
    return result_reply(id, std::move(result));
}

Json Dispatcher::configure_runtime_request(const Json& id, const Json& header,
                                           std::string_view device_name) const {
    if (!deps_.configure_runtime) {
        return error_reply(id, "Update " + std::string(identity().product) +
                                   " on the host to change its runtime.");
    }
    if (has_other_keys(header, {"id", "op", "family", "model", "settings"}) ||
        !(nonempty_string(get(header, "family")) && nonempty_string(get(header, "model"))) ||
        !get(header, "settings").is_object()) {
        return coded_reply(id, "bad_request", "Invalid runtime request.");
    }
    Json engine;
    try {
        engine = deps_.configure_runtime(get(header, "family").get<std::string>(),
                                         get(header, "model").get<std::string>(),
                                         get(header, "settings"), std::string(device_name));
    } catch (const std::exception& error) {
        logger.warning("Runtime change from {} failed", device_name);
        return error_reply(id, detail::error_text(error));
    }
    Json info = Json::object();
    info["by"] = std::string(device_name);
    emit("engine", std::move(info));
    Json result = Json::object();
    result["engine"] = std::move(engine);
    result["runtime"] = runtime();
    return result_reply(id, std::move(result));
}

Json Dispatcher::switch_model(const Json& id, const Json& header,
                              std::string_view device_name) const {
    // Blocks this connection until the load settles, which is what lets the client show one
    // "Switching..." state and then the engine it switched to.
    const Json& family = get(header, "family");
    const Json& model = get(header, "model");
    const bool has_device = header.is_object() && header.contains("device");
    if (has_device) {
        if (!can_manage_models()) {
            return coded_reply(id, "forbidden", "Model management is disabled on the host.");
        }
        if (has_other_keys(header, {"id", "op", "family", "model", "device"}) ||
            !is_device(get(header, "device"))) {
            return error_reply(id, "Invalid model device request.");
        }
    }
    if (!family.is_string() || !nonempty_string(model)) {
        return error_reply(id, "Choose a model to switch to");
    }
    if (!deps_.select_model) {
        return error_reply(id, deps_.host_name + " doesn't let paired computers change its model.");
    }
    Json engine;
    try {
        std::optional<std::string> device;
        if (has_device) {
            device = get(header, "device").get<std::string>();
        }
        engine = deps_.select_model(family.get<std::string>(), model.get<std::string>(),
                                    std::string(device_name), device);
    } catch (const std::exception& error) {
        logger.warning("Model switch from {} failed", device_name);
        return error_reply(id, detail::error_text(error));
    }
    const Json& label = get(engine, "label");
    activity_.switched(device_name,
                       detail::truthy(label) ? detail::py_str(label) : model.get<std::string>());
    Json info = Json::object();
    info["family"] = family;
    info["model"] = model;
    info["by"] = std::string(device_name);
    emit("engine", std::move(info));
    Json result = Json::object();
    result["engine"] = std::move(engine);
    result["models"] = model_list();
    return result_reply(id, std::move(result));
}

} // namespace fv::remote_host
