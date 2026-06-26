#pragma once
#include "motor.hpp"

namespace tapefs { namespace firmware {

struct TapeHeadConfig {
    double blockSpacingMM  = 12.0;
    double leaderLengthMM  = 300.0;
    double totalTapeMM     = 90000.0;
    double motorStartDelay = 0.3;
};

class TapeHead {
public:
    explicit TapeHead(TapeHeadConfig cfg = {}) : cfg_(cfg) {}

    void attachMotor(Motor* m) { motor_ = m; }
    double positionMM() const { return motor_ ? motor_->positionMM() : 0; }
    int currentBlock() const;
    double blockToMM(int blockNo) const { return cfg_.leaderLengthMM + blockNo * cfg_.blockSpacingMM; }
    double travelTimeMs(double fromMM, double toMM) const;

private:
    TapeHeadConfig cfg_;
    Motor* motor_ = nullptr;
};

}}
