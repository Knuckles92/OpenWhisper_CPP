// The urllib.parse pieces the redirect rules rest on. Expected values are Python 3.12.10's
// (OpenWhisper's interpreter) for the same calls.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "pyport/errors.h"
#include "update/detail/url.h"

namespace detail = fv::update::detail;

TEST_CASE("urljoin resolves a redirect Location as urllib does", "[update]") {
    const std::string base = "https://github.com/example/app/releases/latest";
    CHECK(detail::urljoin(base, "/x;") == "https://github.com/x");
    CHECK(detail::urljoin(base, "..;p") == "https://github.com/example/app/;p");
    CHECK(detail::urljoin(base, "../x;p") == "https://github.com/example/app/x;p");
    CHECK(detail::urljoin(base, "tag/v1;a/b") ==
          "https://github.com/example/app/releases/tag/v1;a/b");
    CHECK(detail::urljoin(base, ";p") == "https://github.com/example/app/releases/;p");
    CHECK(detail::urljoin(base, "?q") == "https://github.com/example/app/releases/latest?q");
    CHECK(detail::urljoin(base, "//github.com//x") == "https://github.com//x");
    CHECK(detail::urljoin(base, "https:x") == "https://github.com/example/app/releases/x");
    CHECK(detail::urljoin(base, "a/./b/../c") == "https://github.com/example/app/releases/a/c");
    CHECK(detail::urljoin(base, ".") == "https://github.com/example/app/releases/");
    CHECK(detail::urljoin("https://h/a/b;p", "?q") == "https://h/a/b;p?q");
}

TEST_CASE("urlunsplit keeps an empty authority before a path starting //", "[update]") {
    const auto unsplit = [](std::string scheme, std::string path) {
        detail::UrlParts parts;
        parts.scheme = std::move(scheme);
        parts.path = std::move(path);
        return detail::urlunsplit(parts);
    };
    CHECK(unsplit("https", "//x") == "https:////x");
    CHECK(unsplit("https", "") == "https://");
    CHECK(unsplit("https", "x") == "https:x");
    CHECK(unsplit("", "//x") == "////x");
}

TEST_CASE("urlsplit refuses what Python refuses", "[update]") {
    CHECK_THROWS_AS(detail::urlsplit("http://[::1/x"), fv::py::ValueError);
    CHECK_THROWS_AS(detail::urlsplit("http://[127.0.0.1]/x"), fv::py::ValueError);
    CHECK_THROWS_AS(detail::urlsplit("http://a[::1]/x"), fv::py::ValueError);
    CHECK_NOTHROW(detail::urlsplit("http://[v1.x]/x"));
    const auto parts = detail::urlsplit("HTTP://User:Pw@[::1%25Eth0]:0080/p?q#f");
    CHECK(parts.scheme == "http");
    CHECK(parts.hostname() == "::1%25Eth0");
    CHECK(parts.port() == 80);
    CHECK(parts.username() == "User");
    CHECK(parts.password() == "Pw");
    CHECK(parts.path == "/p");
    CHECK(parts.query == "q");
    CHECK(parts.fragment == "f");
}
