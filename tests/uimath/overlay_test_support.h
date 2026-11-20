#pragma once

#include <vector>

#include "base/json.h"
#include "golden/golden.h"
#include "uimath/color.h"
#include "uimath/particles.h"

namespace fv::overlay_test {

inline Json rgba_json(const Rgba& color) {
    return Json::array({color.r, color.g, color.b, color.a});
}

inline Json point_json(const PointD& point) {
    return Json::array({point.x, point.y});
}

inline Json points_json(const std::vector<PointD>& points) {
    Json out = Json::array();
    for (const PointD& point : points) {
        out.push_back(point_json(point));
    }
    return out;
}

inline Json particle_json(const Particle& p) {
    return Json::array({p.x, p.y, p.vx, p.vy, p.life, p.color_hue});
}

// Whether the case's recorded result equals actual within the file's tolerance.
inline bool matches_result(const golden::Case& c, const Json& actual) {
    return golden::matches(c.raw().at("result"), actual, c.float_tolerance());
}

} // namespace fv::overlay_test
