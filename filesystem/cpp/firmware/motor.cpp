#include "motor.hpp"
#include <cmath>

namespace tapefs { namespace firmware {

void Motor::setSpeed(double targetMMps, int dir) {
    targetSpeed_ = std::abs(targetMMps);
    direction_ = dir;
    if (dir == 0 || targetSpeed_ < 0.1) {
        targetSpeed_ = 0;
        direction_ = 0;
    }
}

void Motor::tick(double dt) {
    if (direction_ == 0) {
        if (currentSpeed_ > 0) {
            currentSpeed_ -= 8000.0 * dt;
            if (currentSpeed_ < 0) currentSpeed_ = 0;
        }
        wowFlutter_ = 0;
        return;
    }

    double diff = targetSpeed_ - currentSpeed_;
    if (std::abs(diff) < 0.5) {
        currentSpeed_ = targetSpeed_;
    } else if (diff > 0) {
        currentSpeed_ += 5000.0 * dt;
        if (currentSpeed_ > targetSpeed_) currentSpeed_ = targetSpeed_;
    } else {
        currentSpeed_ -= 8000.0 * dt;
        if (currentSpeed_ < targetSpeed_) currentSpeed_ = targetSpeed_;
    }

    wowPhase_     += 2.0 * M_PI * 0.5 * dt;
    flutterPhase_ += 2.0 * M_PI * 4.0 * dt;
    wowFlutter_ = (2.0 / 100.0) * std::sin(wowPhase_)
                + (0.8 / 100.0) * std::sin(flutterPhase_);

    double eff = currentSpeed_ * (1.0 + wowFlutter_);
    if (direction_ < 0) eff = -eff;
    positionMM_ += eff * dt;
    if (positionMM_ < 0) positionMM_ = 0;
}

}} // namespace
