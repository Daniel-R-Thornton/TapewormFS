#include "tape_deck.hpp"
#include <cmath>
#include <chrono>
#include <algorithm>

namespace tapefs { namespace firmware {

TapeDeck::TapeDeck(TapeDeckConfig cfg, TapeMedium* medium)
    : medium_(medium) {
    (void)cfg; // use defaults for now
    head_.attachMotor(&motor_);
}

void TapeDeck::seekToBlock(int blockNo) {
    targetBlock_ = blockNo;
    double fromMM = head_.positionMM();
    double toMM   = head_.blockToMM(blockNo);
    if (std::abs(toMM - fromMM) < 1.0) { state_ = 0; return; }

    seekDurationMs_ = head_.travelTimeMs(fromMM, toMM);
    seekStartUs_ = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (toMM > fromMM) motor_.fastForward(); else motor_.rewind();
    state_ = 1; // kSeeking
}

void TapeDeck::rewind() { seekToBlock(0); }
void TapeDeck::stop() { motor_.stop(); state_ = 0; }

double TapeDeck::blockTimeMs() const {
    return (1024.0 * 8.0 / 200.0) * 1000.0 * 1.3;
}

void TapeDeck::tick() {
    motor_.tick(0.01);
    if (state_ != 1) return;

    auto now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    double elapsedMs = (now - seekStartUs_) / 1000.0;

    if (elapsedMs >= seekDurationMs_) {
        motor_.stop();
        state_ = 0;
    }
}

double TapeHead::travelTimeMs(double fromMM, double toMM) const {
    if (std::abs(toMM - fromMM) < 1.0) return 0;
    return (std::abs(toMM - fromMM) / Motor::kFastSpeedMMps) * 1000.0
           + cfg_.motorStartDelay * 1000.0;
}

int TapeHead::currentBlock() const {
    double pos = positionMM();
    if (pos < cfg_.leaderLengthMM) return 0;
    return static_cast<int>((pos - cfg_.leaderLengthMM) / cfg_.blockSpacingMM);
}

}} // namespace
