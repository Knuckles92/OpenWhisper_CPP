// The API keys page's check of a text-model endpoint's key over the context's HTTP client
// (llm::verify_api_key, scripted by http::FakeHttpClient: no request leaves the machine), and what
// the page says when the context has none.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include <QComboBox>
#include <QLineEdit>

#include "app_pages_support.h"
#include "home_support.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "ui_settings/api_keys_section.h"

using fv::Json;
using namespace fvtest;

namespace {

const std::string kKey = "sk-or-test-secret-value-7890wxyz";

// The API keys page of a HomeRig's window, built, its credentials read, OpenRouter selected.
fv::ApiKeysSection& openRouterPage(HomeRig& rig) {
    rig.dialog().selectDestination(qs(dest::api_keys));
    auto* section = dynamic_cast<fv::ApiKeysSection*>(rig.dialog().pageContent(dest::api_keys));
    REQUIRE(section != nullptr);
    REQUIRE(pumpUntil([section] { return section->stateCurrent(); }));
    QComboBox* combo = section->combo();
    combo->setCurrentIndex(combo->findData(QStringLiteral("OPENROUTER_API_KEY")));
    REQUIRE(pumpUntil([section] { return section->stateCurrent(); }));
    return *section;
}

fv::http::FakeHttpClient& fakeClient(HomeRig& rig) {
    auto* client = dynamic_cast<fv::http::FakeHttpClient*>(&rig.context().http());
    REQUIRE(client != nullptr);
    return *client;
}

} // namespace

TEST_CASE("a text-model key is checked with the context's HTTP client", "[widget]") {
    HomeRig rig;
    fv::http::FakeHttpClient& http = fakeClient(rig);
    http.set_handler([](const fv::http::Request&) {
        fv::http::ScriptedResponse response;
        response.body = R"({"data": {"label": "test"}})";
        return response;
    });
    fv::ApiKeysSection& section = openRouterPage(rig);
    section.edit()->setText(QString::fromStdString(kKey));

    section.testKey();
    REQUIRE(pumpUntil([&section] { return !section.testing(); }));

    // OpenRouter's models list is public, so its key is checked against the key endpoint.
    const auto requests = http.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == "https://openrouter.ai/api/v1/auth/key");
    CHECK(fv::http::find_header(requests[0].headers, "Authorization") == "Bearer " + kKey);
    const QString message = rig.dialog().messageText();
    CHECK(message == QStringLiteral("OpenRouter API key works: openrouter.ai accepted the key."));
    CHECK_FALSE(message.contains(QString::fromStdString(kKey)));
}

TEST_CASE("a rejected text-model key says so without the key", "[widget]") {
    HomeRig rig;
    fakeClient(rig).set_handler([](const fv::http::Request&) {
        fv::http::ScriptedResponse response;
        response.status = 401;
        response.reason = "Unauthorized";
        response.body = R"({"error": {"message": "bad key sk-or-test-secret-value-7890wxyz"}})";
        return response;
    });
    fv::ApiKeysSection& section = openRouterPage(rig);
    section.edit()->setText(QString::fromStdString(kKey));

    section.testKey();
    REQUIRE(pumpUntil([&section] { return !section.testing(); }));

    const QString message = rig.dialog().messageText();
    CHECK(message ==
          QStringLiteral("OpenRouter API key failed: openrouter.ai rejected the key (HTTP 401)."));
    CHECK_FALSE(message.contains(QString::fromStdString(kKey)));
}

TEST_CASE("a text-model key test without an HTTP client says it could not run", "[widget]") {
    // The App pages' rig has no HTTP client in its context.
    AppPagesRig rig;
    rig.dialog().selectDestination(qs(dest::api_keys));
    auto* section = dynamic_cast<fv::ApiKeysSection*>(rig.dialog().pageContent(dest::api_keys));
    REQUIRE(section != nullptr);
    REQUIRE(pumpUntil([section] { return section->stateCurrent(); }));
    QComboBox* combo = section->combo();
    combo->setCurrentIndex(combo->findData(QStringLiteral("OPENROUTER_API_KEY")));
    REQUIRE(pumpUntil([section] { return section->stateCurrent(); }));
    section->edit()->setText(QString::fromStdString(kKey));

    section->testKey();
    REQUIRE(pumpUntil([section] { return !section->testing(); }));

    CHECK(rig.dialog().messageText() ==
          QStringLiteral("OpenRouter API key failed: Verification failed."));
    CHECK(rig.verifiedNow().empty());
}
