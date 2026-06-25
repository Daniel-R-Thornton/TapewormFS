#include "tapefs/firmware/Motor.hpp"
#include <cmath>

namespace tapefs { namespace firmware {

void Motor::setSpeed(double targetMMps, Direction dir) {
    targetSpeed_ = std::abs(targetMMps);
    direction_ = dir;
    if (dir == Direction::kStopped || targetSpeed_ < 0.1) {
        targetSpeed_ = 0;
        direction_ = Direction::kStopped;
    }
}

void Motor::tick(double dt) {
    if (direction_ == Direction::kStopped) {
        if (currentSpeed_ > 0) {
            currentSpeed_ -= cfg_.decelerationMMps2 * dt;
            if (currentSpeed_ < 0) currentSpeed_ = 0;
        }
        wowFlutter_ = 0;
        return;
    }

    double diff = targetSpeed_ - currentSpeed_;
    if (std::abs(diff) < 0.5) {
        currentSpeed_ = targetSpeed_;
    } else if (diff > 0) {
        currentSpeed_ += cfg_.accelerationMMps2 * dt;
        if (currentSpeed_ > targetSpeed_) currentSpeed_ = targetSpeed_;
    } else {
        currentSpeed_ -= cfg_.decelerationMMps2 * dt;
        if (currentSpeed_ < targetSpeed_) currentSpeed_ = targetSpeed_;
    }

    wowPhase_     += 2.0 * M_PI * cfg_.wowFreqHz * dt;
    flutterPhase_ += 2.0 * M_PI * cfg_.flutterFreqHz * dt;
    wowFlutter_ = (cfg_.wowDepthPct / 100.0) * std::sin(wowPhase_)
                + (cfg_.flutterDepthPct / 100.0) * std::sin(flutterPhase_);

    double effectiveSpeed = currentSpeed_ * (1.0 + wowFlutter_);
    if (direction_ == Direction::kReverse) effectiveSpeed = -effectiveSpeed;
    positionMM_ += effectiveSpeed * dt;
    if (positionMM_ < 0) positionMM_ = 0;
}

}} // namespace tapefs::firmware
