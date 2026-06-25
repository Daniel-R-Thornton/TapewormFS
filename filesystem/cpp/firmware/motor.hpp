#pragma once
#include <cmath>

namespace tapefs { namespace firmware {

struct MotorConfig {
    double accelerationMMps2  = 5000.0;
    double decelerationMMps2 = 8000.0;
    double wowDepthPct       = 2.0;
    double wowFreqHz         = 0.5;
    double flutterDepthPct   = 0.8;
    double flutterFreqHz     = 4.0;
};

class Motor {
public:
    static constexpr double kPlaySpeedMMps  = 47.6;
    static constexpr double kFastSpeedMMps  = 476.0;

    explicit Motor(MotorConfig cfg = {}) : cfg_(cfg) {}

    void setSpeed(double targetMMps, int dir);
    void play()        { setSpeed(kPlaySpeedMMps, 1); }
    void fastForward() { setSpeed(kFastSpeedMMps, 1); }
    void rewind()      { setSpeed(kFastSpeedMMps, -1); }
    void stop()        { setSpeed(0, 0); }
    void tick(double dt);

    double currentSpeedMMps()  const { return currentSpeed_; }
    double currentPositionMM() const { return positionMM_; }
    bool   isMoving() const { return direction_ != 0 && currentSpeed_ > 0.5; }
    double effectiveSpeedMMps() const { return currentSpeed_ * (1.0 + wowFlutter_); }

private:
    MotorConfig cfg_;
    double targetSpeed_ = 0, currentSpeed_ = 0, positionMM_ = 0;
    int direction_ = 0;
    double wowPhase_ = 0, flutterPhase_ = 0, wowFlutter_ = 0;
};

}}
