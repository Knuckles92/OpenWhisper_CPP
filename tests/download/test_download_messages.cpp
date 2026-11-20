// The verified downloader's user-facing texts (services/verified_download.py MESSAGES; map a14
// A14-66), word for word. The golden resume table reaches most of them through a scripted
// server; "partial_unreadable" and "finalize" need a disk failure it cannot script.

#include <catch2/catch_test_macros.hpp>

#include "download/verified_download.h"

TEST_CASE("the download messages are OpenWhisper's MESSAGES", "[verified_download]") {
    const fv::download::Messages m;
    CHECK(m.invalid_size == "The download has an invalid size.");
    CHECK(m.unsafe_partial == "The download could not be created safely.");
    CHECK(m.partial_not_regular == "The partial download is not a regular file.");
    CHECK(m.partial_unreadable == "The partial download could not be read.");
    CHECK(m.canceled == "The download was canceled.");
    CHECK(m.bad_resume == "The download server returned an invalid resume response.");
    CHECK(m.bad_length == "The download server returned an invalid download size.");
    CHECK(m.unexpected_length == "The download server returned an unexpected download size.");
    CHECK(m.too_much_data == "The download server returned more data than expected.");
    CHECK(m.incomplete == "The download did not complete ({done} of {total}).");
    CHECK(m.integrity ==
          "The download failed its integrity check and was discarded. Please try again.");
    CHECK(m.finalize == "The verified download could not be finalized. Please retry.");
    CHECK(fv::download::kNetworkTimeout == std::chrono::seconds(30));
    CHECK(fv::download::kDownloading == "downloading");
    CHECK(fv::download::kVerifying == "verifying");
}
