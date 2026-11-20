// golden::rebrand keeps a literal map so the harness needs nothing of fv_base (M0.4); here it is
// checked against what identity() and legacy_identity() actually say.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "base/identity.h"
#include "golden/golden.h"

TEST_CASE("rebrand of OpenWhisper is the product identity names", "[unit]") {
    const fv::AppIdentity& id = fv::identity();
    const fv::AppIdentity& legacy = fv::legacy_identity();
    CHECK(fv::golden::rebrand(legacy.product) == id.product);
    CHECK(fv::golden::rebrand("OpenWhisper") == id.product);
    CHECK(fv::golden::rebrand(legacy.settings_file) == id.settings_file);
    CHECK(fv::golden::rebrand(legacy.database_file) == id.database_file);
    CHECK(fv::golden::rebrand(legacy.log_file) == id.log_file);
    CHECK(fv::golden::rebrand(legacy.lease_file) == id.lease_file);
    CHECK(fv::golden::rebrand(legacy.restore_dir) == id.restore_dir);
    CHECK(fv::golden::rebrand(legacy.backup_format) == id.backup_format);
    CHECK(fv::golden::rebrand(legacy.backup_extension) == id.backup_extension);
    CHECK(fv::golden::rebrand(legacy.env_prefix) == id.env_prefix);
    CHECK(fv::golden::rebrand(legacy.instance_key_prefix) == id.instance_key_prefix);
    CHECK(fv::golden::rebrand(std::string(legacy.product) + " is already running") ==
          std::string(id.product) + " is already running");
}
