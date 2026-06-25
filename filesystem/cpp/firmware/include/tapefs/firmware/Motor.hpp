#pragma once

#include <cstdint>
#include <cmath>

namespace tapefs { namespace firmware {

class Motor {
public:
    static constexpr double kPlaySpeedMMps    = 47.6;
    static constexpr double kFastSpeedMMps    = 476.0;
    static constexpr double kMaxSpeedMMps     = 1000.0;

    enum class Direction { kStopped, kForward, kReverse };

    struct Config {
        double accelerationMMps2   = 5000.0;
        double decelerationMMps2  = 8000.0;
        double maxSpeedMMps       = kMaxSpeedMMps;
        double wowDepthPct        = 2.0;
        double wowFreqHz          = 0.5;
        double flutterDepthPct    = 0.8;
        double flutterFreqHz      = 4.0;
    };

    Motor() : Motor(Config{}) {}
    explicit Motor(Config cfg) : cfg_(cfg) {}

    void setSpeed(double targetMMps, Direction dir);
    void play()      { setSpeed(kPlaySpeedMMps, Direction::kForward); }
    void fastForward(){ setSpeed(kFastSpeedMMps, Direction::kForward); }
    void rewind()    { setSpeed(kFastSpeedMMps, Direction::kReverse); }
    void stop()      { setSpeed(0, Direction::kStopped); }
    void tick(double dtSeconds);

    double currentSpeedMMps() const { return currentSpeed_; }
    double currentPositionMM() const { return positionMM_; }
    Direction direction() const { return direction_; }
    bool    isMoving()    const { return direction_ != Direction::kStopped; }
    bool    isAtSpeed()   const { return std::abs(currentSpeed_ - targetSpeed_) < 1.0; }
    double  effectiveSpeedMMps() const { return currentSpeed_ * (1.0 + wowFlutter_); }

private:
    Config  cfg_;
    double  targetSpeed_  = 0;
    double  currentSpeed_ = 0;
    double  positionMM_   = 0;
    Direction direction_  = Direction::kStopped;
    double  wowPhase_     = 0;
    double  flutterPhase_ = 0;
    double  wowFlutter_   = 0;
};

}}
