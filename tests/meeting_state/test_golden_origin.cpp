// Golden vectors of the paired-computer queries
// (tests/golden/meeting.persist.repository_origin.json,
// tools/parity/cases/meeting.persist.repository_origin.py): each case's `setup` written into a
// fresh database, then the function run over it as OpenWhisper's SqlMeetingRepository ran it.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "golden/golden.h"
#include "meeting_state/origin_rows.h"
#include "meeting_state/sql_repository.h"
#include "origin_support.h"
#include "pyport/errors.h"
#include "sql/db.h"

namespace {

using fv::Json;
using fv::meeting_state::origin_test::b64_tagged;
using fv::meeting_state::origin_test::b64_untagged;
using fv::meeting_state::origin_test::dump;
using fv::meeting_state::origin_test::OriginDatabase;

constexpr std::string_view kModule = "meeting.persist.repository_origin";

Json owners_json(const std::vector<fv::meeting_state::RecordOwner>& owners) {
    Json result = Json::array();
    for (const auto& owner : owners) {
        result.push_back({{"id", owner.id}, {"name", owner.name}, {"count", owner.count}});
    }
    return result;
}

} // namespace

TEST_CASE("golden export_meeting_rows", "[golden]") {
    fv::golden::for_each(kModule, "export_meeting_rows", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        const Json rows =
            fv::meeting_state::export_meeting_rows(*db.database, c.arg<std::string>(1));
        INFO(c.label());
        CHECK(fv::golden::equal(c.result<Json>(), b64_tagged(rows)));
    });
}

TEST_CASE("golden replace_meeting_rows", "[golden]") {
    fv::golden::for_each(kModule, "replace_meeting_rows", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        const Json expected = c.result<Json>();
        Json raised = nullptr;
        Json message = nullptr;
        try {
            fv::meeting_state::replace_meeting_rows(*db.database, c.arg<std::string>(1),
                                                    b64_untagged(c.arg<Json>(2)),
                                                    b64_untagged(c.arg<Json>(3)));
        } catch (const fv::py::PyError& error) {
            raised = std::string(error.type_name());
            // The case file records str(exc) of a TypeError or KeyError only.
            if (error.type_name() == "TypeError" || error.type_name() == "KeyError") {
                message = error.what();
            }
        } catch (const fv::sql::Error&) {
            // SQLAlchemy reports a constraint SQLite refused as IntegrityError.
            raised = "IntegrityError";
        }
        INFO(c.label());
        CHECK(raised == expected.at("raised"));
        CHECK(message == expected.at("message"));
        CHECK(fv::golden::equal(expected.at("after"), dump(*db.database)));
    });
}

TEST_CASE("golden record_owners", "[golden]") {
    fv::golden::for_each(kModule, "record_owners", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        INFO(c.label());
        CHECK(fv::golden::equal(c.result<Json>(),
                                owners_json(fv::meeting_state::record_owners(*db.database))));
    });
}

TEST_CASE("golden rename_origin", "[golden]") {
    fv::golden::for_each(kModule, "rename_origin", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        const int changed = fv::meeting_state::rename_origin(*db.database, c.arg<std::string>(1),
                                                             c.arg<std::string>(2));
        const Json actual{{"changed", changed},
                          {"owners", owners_json(fv::meeting_state::record_owners(*db.database))}};
        INFO(c.label());
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden origin_spools", "[golden]") {
    fv::golden::for_each(kModule, "origin_spools", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        Json actual = Json::array();
        for (const auto& spool :
             fv::meeting_state::origin_spools(*db.database, c.arg<std::string>(1))) {
            actual.push_back(Json::array({spool.meeting_id, spool.spool_dir}));
        }
        INFO(c.label());
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("golden count_unfinished_chunks", "[golden]") {
    fv::golden::for_each(kModule, "count_unfinished_chunks", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        const std::string meeting_id = c.arg<std::string>(1);
        INFO(c.label());
        CHECK(fv::meeting_state::count_unfinished_chunks(*db.database, meeting_id) ==
              c.result<std::int64_t>());
        // The repository's own count runs the same query.
        fv::meeting_state::SqlMeetingRepository repository(*db.database);
        CHECK(repository.count_unfinished_chunks(meeting_id) == c.result<std::int64_t>());
    });
}

TEST_CASE("golden list_past_meeting_summaries by origin", "[golden]") {
    using Origin = fv::meeting_state::PastMeetingQuery::Origin;
    fv::golden::for_each(kModule, "list_past_meeting_ids", [](const fv::golden::Case& c) {
        OriginDatabase db(c.arg<Json>(0));
        const auto origin = c.arg<std::optional<std::string>>(1);
        fv::meeting_state::PastMeetingQuery query;
        if (!origin) {
            query.origin = Origin::Local;
        } else if (*origin != "...") {
            query.origin = Origin::Device;
            query.origin_device_id = *origin;
        }
        fv::meeting_state::SqlMeetingRepository repository(*db.database);
        std::vector<std::string> ids;
        for (const Json& meeting : repository.list_past_meeting_summaries(query)) {
            ids.push_back(meeting.at("id").get<std::string>());
        }
        INFO(c.label());
        CHECK(ids == c.result<std::vector<std::string>>());
    });
}
