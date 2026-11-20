// The golden vectors of ui_qt.utils.list_reconcile: reconcile_cards run on a fake layout (see
// tools/parity/cases/ui_qt.utils.list_reconcile.py for the argument and result shapes).
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "uimath/list_reconcile.h"

namespace {

std::vector<fv::ReconcileItem> items(const fv::Json& pairs) {
    std::vector<fv::ReconcileItem> out;
    for (const fv::Json& pair : pairs) {
        out.push_back({pair.at(0).get<std::string>(), pair.at(1)});
    }
    return out;
}

fv::Json planJson(const fv::ReconcilePlan& plan) {
    fv::Json entries = fv::Json::array();
    for (const auto& entry : plan.entries) {
        entries.push_back(fv::Json::array({entry.id, entry.reused}));
    }
    fv::Json fingerprints = fv::Json::array();
    for (const auto& [id, fingerprint] : plan.fingerprints) {
        fingerprints.push_back(fv::Json::array({id, fingerprint}));
    }
    fv::Json result = fv::Json::object();
    result["entries"] = entries;
    result["removed"] = plan.removed;
    result["inserted"] = plan.inserted;
    result["fingerprints"] = fingerprints;
    return result;
}

} // namespace

TEST_CASE("reconcile_plan replays the golden vectors", "[golden][list_reconcile]") {
    fv::golden::for_each("ui_qt.utils.list_reconcile", "reconcile_cards",
                         [](const fv::golden::Case& c) {
                             const auto layout = c.arg<std::vector<std::optional<std::string>>>(0);
                             std::map<std::string, fv::Json> previous;
                             for (const fv::ReconcileItem& item : items(c.args().at(1))) {
                                 previous[item.id] = item.fingerprint;
                             }
                             const fv::ReconcilePlan plan =
                                 fv::reconcile_plan(layout, previous, items(c.args().at(2)));
                             CHECK(fv::golden::equal(c.raw().at("result"), planJson(plan)));
                         });
}

TEST_CASE("an unchanged list keeps every card where it is", "[list_reconcile]") {
    const std::map<std::string, fv::Json> previous{{"a", 1}, {"b", 2}};
    const fv::ReconcilePlan plan = fv::reconcile_plan({"a", "b"}, previous, {{"a", 1}, {"b", 2}});
    REQUIRE(plan.entries.size() == 2);
    CHECK(plan.entries[0].reused);
    CHECK(plan.entries[1].reused);
    CHECK(plan.removed.empty());
    CHECK(plan.inserted.empty());
}

TEST_CASE("a changed fingerprint replaces only that card", "[list_reconcile]") {
    const std::map<std::string, fv::Json> previous{{"a", 1}, {"b", 2}};
    const fv::ReconcilePlan plan = fv::reconcile_plan({"a", "b"}, previous, {{"a", 1}, {"b", 3}});
    CHECK(plan.entries[0].reused);
    CHECK_FALSE(plan.entries[1].reused);
    CHECK(plan.removed == std::vector<std::size_t>{1});
    CHECK(plan.inserted == std::vector<std::size_t>{1});
}
