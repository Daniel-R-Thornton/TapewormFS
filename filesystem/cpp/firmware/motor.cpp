#include "motor.hpp"
#include <cmath>
#include <algorithm>

namespace tapefs { namespace firmware {

void Motor::set(MotorDir dir, MotorSpeed speed) {
    dir_   = dir;
    speed_ = speed;
    if (speed == MotorSpeed::kOff) spinUpElapsed_ = 0;
    else spinUpElapsed_ = 0; // reset spin-up timer
}

double Motor::currentSpeedMMps() const {
    if (speed_ == MotorSpeed::kOff) return 0;

    // Spin-up: gradually reach full speed over ~150ms
    double fraction = std::min(1.0, spinUpElapsed_ / kSpinUpMs);
    double base = (speed_ == MotorSpeed::kFast) ? kFastSpeedMMps : kPlaySpeedMMps;
    return base * fraction;
}

double Motor::effectiveSpeed() const {
    double nominal = currentSpeedMMps();
    double flutter = (2.0 / 100.0) * std::sin(wowPhase_)
                   + (0.8 / 100.0) * std::sin(flutterPhase_);
    return nominal * (1.0 + flutter);
}

void Motor::tick(double dt) {
    if (speed_ != MotorSpeed::kOff) {
        spinUpElapsed_ += dt * 1000.0; // convert to ms
    }

    // Wow/flutter phase
    wowPhase_     += 2.0 * M_PI * 0.5 * dt;
    flutterPhase_ += 2.0 * M_PI * 4.0 * dt;

    // Update position
    double eff = effectiveSpeed();
    if (dir_ == MotorDir::kReverse) eff = -eff;
    posMm_ += eff * dt * (speed_ != MotorSpeed::kOff ? 1.0 : 0.0);
    if (posMm_ < 0) posMm_ = 0;
}

}} // namespace
