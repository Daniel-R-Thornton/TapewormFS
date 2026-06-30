#include "modem_encoder.hpp"
#include "esp32_hal.hpp"
#include <cmath>
#include <cstring>

namespace tapefs { namespace firmware {

static constexpr double kTones[8] = {400, 600, 800, 1000, 1150, 1300, 1450, 1550};

float ModemEncoder::pilotSample() {
    float v = std::sin(2.0 * M_PI * pilotPhase_) * cfg_.pilotAmplitude;
    pilotPhase_ += cfg_.pilotFreqHz / cfg_.sampleRate;
    if (pilotPhase_ >= 1.0) pilotPhase_ -= 1.0;
    return v;
}

float ModemEncoder::toneSample(double freqHz) {
    // Reset phase each time frequency changes — decoder expects clean symbols
    static double tonePhase = 0;
    static double lastFreq = 0;
    if (freqHz <= 0) { tonePhase = 0; lastFreq = 0; return 0; }
    if (freqHz != lastFreq) { tonePhase = 0; lastFreq = freqHz; }
    float v = std::sin(2.0 * M_PI * tonePhase);
    tonePhase += freqHz / cfg_.sampleRate;
    if (tonePhase >= 1.0) tonePhase -= 1.0;
    return v;
}

void ModemEncoder::advancePhase() {
    Phase next = static_cast<Phase>(static_cast<int>(phase_) + 1);
    phase_ = next;
    samplesInPhase_ = 0;
}

void ModemEncoder::startEncoding(const std::vector<uint8_t>& data) {
    sps_ = cfg_.sampleRate / cfg_.symbolsPerSec;
    guard_ = 0;  // no guard

    phase_ = Phase::kLeader;
    samplesInPhase_ = 0;
    pilotPhase_ = 0;
    symbolIndex_ = 0;

    symbols_.clear();
    for (auto byte : data) {
        int hi = (byte >> 5) & 0x07;  // top 3 bits = one symbol
        symbols_.push_back(hi);
    }
}

bool ModemEncoder::generateSample() {
    float output = pilotSample();

    switch (phase_) {
    case Phase::kLeader:
        // Just pilot tone for 1 second
        samplesInPhase_++;
        if (samplesInPhase_ >= cfg_.sampleRate) advancePhase();
        break;

    case Phase::kSync:
        output += toneSample(kTones[0]); // sync = tone 0
        samplesInPhase_++;
        if (samplesInPhase_ >= sps_ * cfg_.syncSymbols) advancePhase();
        break;

    case Phase::kData:
        if (symbolIndex_ < (int)symbols_.size()) {
            output += toneSample(kTones[symbols_[symbolIndex_] & 0x07]);
        }
        samplesInPhase_++;
        if (samplesInPhase_ >= sps_) {
            samplesInPhase_ = 0;
            symbolIndex_++;
            if (symbolIndex_ >= (int)symbols_.size()) {
                phase_ = Phase::kDone;
                samplesInPhase_ = 0;
            }
        }
        break;

    case Phase::kGuard:
        // Just pilot
        samplesInPhase_++;
        if (samplesInPhase_ >= guard_) advancePhase();
        break;

    case Phase::kDone:
        if (onDone_) onDone_();
        return false;
    }

    hal::dacWriteFloat(output);
    return phase_ != Phase::kDone;
}

}} // namespace
