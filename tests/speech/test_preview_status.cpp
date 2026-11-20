// preview_unavailable_reason (services/runtime/streaming.py 29-50), with the lead's wording.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "speech/preview_status.h"

using fv::speech::preview_unavailable_reason;

TEST_CASE("Parakeet previews; other engines say what the preview needs", "[preview_status]") {
    CHECK(preview_unavailable_reason("parakeet").empty());
    CHECK(preview_unavailable_reason("api") == "Live preview needs Parakeet");
    CHECK(preview_unavailable_reason("") == "Live preview needs Parakeet");
    // Dropped engines are not engines here.
    CHECK(preview_unavailable_reason("local_whisper") == "Live preview needs Parakeet");
    CHECK(preview_unavailable_reason("nemotron") == "Live preview needs Parakeet");
    CHECK(fv::speech::streams_preview("parakeet"));
    CHECK_FALSE(fv::speech::streams_preview("api"));
}

TEST_CASE("the remote engine previews with whatever the paired host runs", "[preview_status]") {
    // Until the host reports its family, setup cannot tell, and checks again later.
    CHECK(preview_unavailable_reason("remote").empty());
    CHECK(preview_unavailable_reason("remote", "studio").empty());
    CHECK(preview_unavailable_reason("remote", "studio", "parakeet").empty());
    // An OpenWhisper host's previewing families.
    CHECK(preview_unavailable_reason("remote", "studio", "nemotron").empty());
    CHECK(preview_unavailable_reason("remote", "studio", "parakeet_unified").empty());
    CHECK(preview_unavailable_reason("remote", "studio", "parakeet_mlx").empty());
    CHECK(preview_unavailable_reason("remote", "studio", "whisper") ==
          "Live preview needs Parakeet on studio");
    CHECK(preview_unavailable_reason("remote", "", "moonshine") ==
          "Live preview needs Parakeet on the paired computer");
}
