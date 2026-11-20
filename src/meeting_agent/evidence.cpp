#include "meeting_agent/evidence.h"

#include <map>
#include <set>

#include "pyport/difflib.h"

namespace fv::meeting_agent {

std::optional<std::string> repair_one(std::string_view bad_id,
                                      const std::vector<std::string>& citable) {
    if (!bad_id.starts_with("sg_")) {
        return std::nullopt;
    }
    std::vector<const std::string*> prefix_hits;
    for (const auto& id : citable) {
        if (std::string_view(id).starts_with(bad_id)) {
            prefix_hits.push_back(&id);
        }
    }
    if (prefix_hits.size() == 1) {
        return *prefix_hits.front();
    }
    std::vector<const std::string*> contain_hits;
    for (const auto& id : citable) {
        if (id.find(bad_id) != std::string::npos) {
            contain_hits.push_back(&id);
        }
    }
    if (contain_hits.size() == 1) {
        return *contain_hits.front();
    }
    const auto matches = py::get_close_matches(bad_id, citable, 2, 0.5);
    if (matches.empty()) {
        return std::nullopt;
    }
    const double best = py::SequenceMatcher(bad_id, matches[0]).ratio();
    const double second =
        matches.size() > 1 ? py::SequenceMatcher(bad_id, matches[1]).ratio() : 0.0;
    if (best >= FUZZY_MIN_RATIO && (best - second) >= FUZZY_MARGIN) {
        return matches[0];
    }
    return std::nullopt;
}

std::pair<Json, int> repair_evidence_ids(const Json& ops, const std::vector<std::string>& citable,
                                         const std::function<bool(std::string_view)>& exists) {
    std::vector<std::string> unique;
    std::set<std::string, std::less<>> seen;
    for (const auto& id : citable) {
        if (seen.insert(id).second) {
            unique.push_back(id);
        }
    }
    std::map<std::string, std::optional<std::string>, std::less<>> cache;
    int repaired = 0;
    Json out = Json::array();
    if (!ops.is_array()) {
        return {out, 0};
    }
    for (const auto& op : ops) {
        const auto evidence = op.is_object() ? op.find("evidence") : op.end();
        if (!op.is_object() || evidence == op.end() || !evidence->is_array() || evidence->empty()) {
            out.push_back(op);
            continue;
        }
        Json fixed_evidence = Json::array();
        bool changed = false;
        for (const auto& entry : *evidence) {
            if (!entry.is_string()) {
                changed = true;
                continue;
            }
            const auto& id = entry.get_ref<const std::string&>();
            if (exists(id)) {
                fixed_evidence.push_back(entry);
                continue;
            }
            auto found = cache.find(id);
            if (found == cache.end()) {
                found = cache.emplace(id, repair_one(id, unique)).first;
            }
            if (found->second) {
                changed = true;
                ++repaired;
                fixed_evidence.push_back(*found->second);
            } else {
                fixed_evidence.push_back(entry);
            }
        }
        if (changed) {
            Json copy = op;
            copy["evidence"] = std::move(fixed_evidence);
            out.push_back(std::move(copy));
        } else {
            out.push_back(op);
        }
    }
    return {out, repaired};
}

} // namespace fv::meeting_agent
