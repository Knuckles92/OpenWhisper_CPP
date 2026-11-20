// The proxy choice as urllib.request makes it. Expected values are what CPython 3.12.10's
// getproxies_environment, getproxies_registry's parse, proxy_bypass_environment and
// _proxy_bypass_winreg_override return for the same inputs.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "http/proxy.h"

using fv::http::ProxyMap;
using fv::http::ProxySettings;

namespace {

using Environ = std::vector<std::pair<std::string, std::string>>;

} // namespace

TEST_CASE("the environment's <scheme>_proxy variables name proxies", "[proxy]") {
    CHECK(fv::http::proxies_from_environment(
              Environ{{"HTTP_PROXY", "http://a:1"}, {"HTTPS_PROXY", "http://b:2"}}, false) ==
          ProxyMap{{"http", "http://a:1"}, {"https", "http://b:2"}});
    // A CGI script ignores HTTP_PROXY, which a client can set through a "Proxy:" header.
    CHECK(fv::http::proxies_from_environment(Environ{{"HTTP_PROXY", "http://a:1"}}, true).empty());
    CHECK(fv::http::proxies_from_environment(
              Environ{{"NO_PROXY", "localhost,.example.com"}, {"FTP_PROXY", ""}}, false) ==
          ProxyMap{{"no", "localhost,.example.com"}});
    CHECK(fv::http::proxies_from_environment(
              Environ{
                  {"X_PROXY", "x"}, {"PROXY", "y"}, {"_PROXY", "z"}, {"ALL_PROXY", "socks5://s:1"}},
              false) == ProxyMap{{"x", "x"}, {"", "z"}, {"all", "socks5://s:1"}});
}

TEST_CASE("lower-case variables win over upper-case ones where names keep their case", "[proxy]") {
    CHECK(fv::http::proxies_from_environment(
              Environ{{"http_proxy", "http://lower:1"}, {"HTTP_PROXY", "http://upper:1"}}, false) ==
          ProxyMap{{"http", "http://lower:1"}});
    // An empty lower-case variable removes the scheme, even with REQUEST_METHOD set.
    CHECK(fv::http::proxies_from_environment(
              Environ{{"HTTPS_PROXY", "http://upper:1"}, {"https_proxy", ""}}, false)
              .empty());
    CHECK(fv::http::proxies_from_environment(Environ{{"http_proxy", "http://lower:1"}}, true) ==
          ProxyMap{{"http", "http://lower:1"}});
}

TEST_CASE("the registry's ProxyServer is read as urllib reads it", "[proxy]") {
    using Case = std::pair<std::string, ProxyMap>;
    const std::vector<Case> cases = {
        {"proxy.corp:8080",
         {{"http", "http://proxy.corp:8080"},
          {"https", "http://proxy.corp:8080"},
          {"ftp", "http://proxy.corp:8080"}}},
        {"http=a:1;https=b:2", {{"http", "http://a:1"}, {"https", "http://b:2"}}},
        {"https=https://b:2", {{"https", "https://b:2"}}},
        {"socks=s:1080",
         {{"socks", "socks://s:1080"}, {"http", "socks4://s:1080"}, {"https", "socks4://s:1080"}}},
        {"http=a:1;socks=s:1080",
         {{"http", "http://a:1"}, {"socks", "socks://s:1080"}, {"https", "socks4://s:1080"}}},
        {"http=a:1;;https=b:2", {{"http", "http://a:1"}}},
        {"http=a:1;bogus", {{"http", "http://a:1"}}},
        {"", {{"http", "http://"}, {"https", "http://"}, {"ftp", "http://"}}},
        {"socks5://x:1",
         {{"http", "socks5://x:1"}, {"https", "socks5://x:1"}, {"ftp", "socks5://x:1"}}},
    };
    for (const auto& [server, expected] : cases) {
        CAPTURE(server);
        CHECK(fv::http::proxies_from_registry(server) == expected);
    }
}

TEST_CASE("no_proxy bypasses hosts by DNS suffix", "[proxy]") {
    struct Case {
        std::string host;
        ProxyMap proxies;
        bool bypass;
    };
    const std::vector<Case> cases = {
        {"example.com", {{"no", "example.com"}}, true},
        {"www.example.com", {{"no", "example.com"}}, true},
        {"www.example.com:8080", {{"no", ".example.com"}}, true},
        {"notexample.com", {{"no", "example.com"}}, false},
        {"Example.COM", {{"no", " EXAMPLE.com , other"}}, true},
        {"anything", {{"no", "*"}}, true},
        {"host", {{"no", ""}}, false},
        {"host", {}, false},
        {"127.0.0.1:8080", {{"no", "127.0.0.1"}}, true},
        {"[::1]:8080", {{"no", "[::1]"}}, true},
    };
    for (const auto& c : cases) {
        CAPTURE(c.host);
        CHECK(fv::http::proxy_bypass_environment(c.host, c.proxies) == c.bypass);
    }
}

TEST_CASE("ProxyOverride bypasses by pattern and <local>", "[proxy]") {
    struct Case {
        std::string host;
        std::string override_list;
        bool bypass;
    };
    const std::vector<Case> cases = {
        {"intranet", "<local>", true},
        {"intra.net", "<local>", false},
        {"intranet:8080", "<local>", true},
        {"www.example.net", "*.example.net", true},
        {"WWW.Example.NET", "*.example.net", true},
        {"example.net", "*.example.net", false},
        {"192.168.0.1", "192.168.*", true},
        {"10.0.0.1", "www.example.com; 10.0.0.?", true},
        {"10.0.0.12", "10.0.0.?", false},
        {"a.b", "[ab].b", true},
        {"c.b", "[!ab].b", true},
        {"x", "[x", false},
        {"[x", "[x", true},
        {"host", "", false},
    };
    for (const auto& c : cases) {
        CAPTURE(c.host, c.override_list);
        CHECK(fv::http::proxy_bypass_registry_override(c.host, c.override_list) == c.bypass);
    }
}

TEST_CASE("a request's proxy comes from the environment, else the registry", "[proxy]") {
    ProxySettings settings;
    settings.registry = {{"http", "http://reg:1"}, {"https", "http://reg:2"}};
    settings.registry_override = "<local>;*.corp";
    CHECK(fv::http::proxy_for_url("https://huggingface.co/x", settings) == "http://reg:2");
    CHECK(fv::http::proxy_for_url("HTTP://example.com/x", settings) == "http://reg:1");
    CHECK_FALSE(fv::http::proxy_for_url("https://intranet/x", settings));
    CHECK_FALSE(fv::http::proxy_for_url("https://files.corp:8443/x", settings));
    CHECK_FALSE(fv::http::proxy_for_url("ftp://example.com/x", ProxySettings{}));

    // Any environment entry replaces the registry wholesale, its bypass rules included.
    settings.environment = {{"https", "http://env:3"}, {"no", "example.com"}};
    CHECK(fv::http::proxy_for_url("https://files.corp/x", settings) == "http://env:3");
    CHECK_FALSE(fv::http::proxy_for_url("https://www.example.com/x", settings));
    CHECK_FALSE(fv::http::proxy_for_url("http://huggingface.co/x", settings));
}

TEST_CASE("the system proxy settings can be read", "[proxy][platform]") {
    // Whatever this machine has: the read must not fail, and an environment entry never names
    // the "proxy" scheme of a bare PROXY variable.
    const auto settings = fv::http::read_system_proxy_settings();
    CHECK(settings.environment.find("proxy") == settings.environment.end());
}
