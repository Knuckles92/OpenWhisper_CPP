#include "agentapi/api_errors.h"

#include <cctype>
#include <utility>

#include "pyport/json.h"

namespace fv::agentapi {

namespace {

std::string lowered(std::string name) {
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name;
}

} // namespace

std::string render_json(const Json& value) {
    py::DumpOptions options;
    options.ensure_ascii = false;
    options.item_sep = ",";
    options.key_sep = ":";
    return py::dumps(value, options);
}

ApiResponse json_response(int status, const Json& value, net::Headers extra) {
    ApiResponse response;
    response.status = status;
    for (auto& [name, header_value] : extra) {
        response.headers.emplace_back(lowered(std::move(name)), std::move(header_value));
    }
    response.headers.emplace_back("content-type", "application/json");
    response.body = render_json(value);
    return response;
}

ApiResponse error_response(int status, std::string_view code, std::string_view message,
                           net::Headers extra) {
    Json error = Json::object();
    error["code"] = std::string(code);
    error["message"] = std::string(message);
    Json body = Json::object();
    body["error"] = std::move(error);
    return json_response(status, body, std::move(extra));
}

ApiResponse internal_error_response() {
    ApiResponse response;
    response.status = 500;
    response.headers.emplace_back("content-type", "text/plain; charset=utf-8");
    response.body = "Internal Server Error";
    return response;
}

} // namespace fv::agentapi
