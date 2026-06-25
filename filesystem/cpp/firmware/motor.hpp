#pragma once
/**
 * Motor — 3-wire cassette deck motor.
 *
 * Real hardware: just FORWARD, REVERSE, FAST, OFF.
 * No PWM, no acceleration ramps.  Snap to speed or stop.
 * Wow/flutter still happens (mechanical tape system).
 */

namespace tapefs { namespace firmware {

enum class MotorSpeed { kOff, kPlay, kFast };
enum class MotorDir   { kForward = 1, kReverse = -1 };

class Motor {
public:
    static constexpr double kPlaySpeedMMps = 47.6;   // standard cassette
    static constexpr double kFastSpeedMMps = 476.0;  // 10× for FF/rewind
    static constexpr double kSpinUpMs      = 150.0;  // ms to reach speed

    Motor() = default;

    /// Set direction and speed.  Simple 3-wire control.
    void set(MotorDir dir, MotorSpeed speed);

    void play()       { set(MotorDir::kForward, MotorSpeed::kPlay); }
    void fastForward(){ set(MotorDir::kForward, MotorSpeed::kFast); }
    void rewind()    { set(MotorDir::kReverse, MotorSpeed::kPlay); }
    void fastRev()    { set(MotorDir::kReverse, MotorSpeed::kFast); }
    void stop()       { set(MotorDir::kForward, MotorSpeed::kOff); }

    /// Advance by dt seconds.  Updates position (mm from BOT).
    void tick(double dt);

    // ---- queries ------------------------------------------------- //

    double currentSpeedMMps() const;
    double positionMM()  const { return posMm_; }
    bool   isMoving()    const { return speed_ != MotorSpeed::kOff; }
    double effectiveSpeed() const;

private:
    MotorSpeed speed_ = MotorSpeed::kOff;
    MotorDir   dir_   = MotorDir::kForward;
    double     posMm_ = 0;

    // Spinning state: real motor takes ~150ms to reach speed
    double spinUpElapsed_ = 0;

    // Wow/flutter state
    double wowPhase_     = 0;
    double flutterPhase_ = 0;
};

}} // namespace
