#include "uimath/fizz.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "uimath/particles.h"
#include "uimath/voice_ribbon.h"

namespace fv {

FizzSim::FizzSim(double width, std::mt19937 rng) : width_(width), initial_rng_(rng), rng_(rng) {
    reset();
}

void FizzSim::reset() {
    h_.fill(0.0);
    v_.fill(0.0);
    bubbles_.clear();
    splashes_.clear();
    t_ = 0.0;
    carry_ = 0.0;
    rng_ = initial_rng_;
    work_from_.reset();
}

double FizzSim::pitch() const {
    return (width_ - 2 * kRibbonEdge) / kLanes;
}

double FizzSim::lane_x(int lane) const {
    return kRibbonEdge + pitch() * (lane + 0.5);
}

void FizzSim::spawn(double rate, double speed, double dt) {
    carry_ += rate * dt;
    while (carry_ >= 1.0) {
        carry_ -= 1.0;
        Bubble b;
        b.lane = random_below(rng_, kLanes);
        b.x = lane_x(b.lane) + random_uniform(rng_, -0.3, 0.3) * (pitch() - 8.0);
        b.y = kBase;
        b.vy = -speed * random_uniform(rng_, 0.8, 1.2);
        b.r = random_uniform(rng_, 1.4, 3.0);
        bubbles_.push_back(b);
    }
}

void FizzSim::rise(double dt, bool lift) {
    std::vector<Bubble> alive;
    for (Bubble& b : bubbles_) {
        b.y += b.vy * dt;
        const double surface = kBase - h_[static_cast<std::size_t>(b.lane)];
        if (b.y - b.r <= surface) {
            if (lift) {
                v_[static_cast<std::size_t>(b.lane)] +=
                    kKick * (std::fabs(b.vy) / 300.0) * (b.r / 2.4);
            }
            splashes_.push_back(Splash{t_, b.x, surface});
        } else if (b.y > -10.0) {
            alive.push_back(b);
        }
    }
    bubbles_ = std::move(alive);
    std::erase_if(splashes_, [this](const Splash& s) { return !(t_ - s.start < kSplashS); });
}

void FizzSim::advance(double dt, double shown) {
    t_ += dt;
    spawn(10.0 + 230.0 * std::pow(shown, 1.5), 110.0 + 320.0 * shown, dt);
    rise(dt, true);
    for (std::size_t i = 0; i < kLanes; ++i) {
        // The fuller a glass, the harder it drains, so each loudness has its own level to
        // slosh around instead of every word filling it.
        v_[i] -= (kGravity + kLeak * h_[i]) * dt;
        v_[i] *= std::exp(-dt / kSloshS);
        h_[i] = detail::py_max(0.0, detail::py_min(kMaxHeight, h_[i] + v_[i] * dt));
        if (h_[i] == 0.0 || h_[i] == kMaxHeight) {
            v_[i] = 0.0;
        }
    }
}

void FizzSim::begin_work() {
    work_from_ = h_;
}

void FizzSim::advance_work(double dt, double rate) {
    t_ += dt;
    spawn(40.0 * rate, 180.0 * rate, dt);
    rise(dt, false);
}

const FizzSim::Levels& FizzSim::work_levels(double morph, double clock) {
    const Levels start = work_from_ ? *work_from_ : h_;
    for (int i = 0; i < kLanes; ++i) {
        const double wave =
            10.0 + 20.0 * (0.5 + 0.5 * std::sin(2.0 * std::numbers::pi *
                                                (static_cast<double>(i) / kLanes - clock * 0.7)));
        h_[static_cast<std::size_t>(i)] = start[static_cast<std::size_t>(i)] +
                                          (wave - start[static_cast<std::size_t>(i)]) * morph;
    }
    return h_;
}

} // namespace fv
