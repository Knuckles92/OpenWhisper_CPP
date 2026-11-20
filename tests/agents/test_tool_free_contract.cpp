#include "agents/drivers.h"
#include "base/json.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("the tool-free task contract requests only the finished text", "[unit]") {
    const auto contract = fv::agents::tool_contract(fv::Json::array(), "Claude Code", "",
                                                    "Return only cleaned text.");
    CHECK(contract.find("no tools") != std::string::npos);
    CHECK(contract.find("patch_state") == std::string::npos);
    CHECK(contract.find("only by calling") == std::string::npos);
    CHECK(contract.ends_with("Return only cleaned text."));
}
