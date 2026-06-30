#include "modem_encoder.hpp"
#include "esp32_hal.hpp"

namespace tapefs { namespace firmware {

void ModemEncoder::advancePhase() {
    phase_ = static_cast<Phase>(static_cast<int>(phase_) + 1);
    samplesInPhase_ = 0;
}

float ModemEncoder::pilotSample() {
    float v = std::sin(2.0 * M_PI * pilotPhase_) * cfg_.pilotAmplitude;
    pilotPhase_ += cfg_.pilotFreqHz / cfg_.sampleRate;
    if (pilotPhase_ >= 1.0) pilotPhase_ -= 1.0;
    return v;
}

float ModemEncoder::toneSample(double freqHz, double phase) {
    return std::sin(2.0 * M_PI * phase);
}

void ModemEncoder::startEncoding(const std::vector<uint8_t>& data) {
    sps_ = cfg_.sampleRate / cfg_.symbolsPerSec;  // 128
    bitsPerFrame_ = cfg_.bitsPerFrame;              // 4

    phase_ = Phase::kLeader;
    samplesInPhase_ = 0;
    pilotPhase_ = 0;
    symbolIndex_ = 0;
    bitPos_ = 0;

    // Convert bytes to bitstream (MSB first)
    bitstream_.clear();
    for (auto byte : data) {
        for (int b = 7; b >= 0; b--) {
            bitstream_.push_back((byte >> b) & 1);
        }
    }
}

bool ModemEncoder::generateSample() {
    float output = pilotSample();

    switch (phase_) {
    case Phase::kLeader:
        // Just pilot for 0.5 seconds
        samplesInPhase_++;
        if (samplesInPhase_ >= cfg_.sampleRate / 2) advancePhase();
        break;

    case Phase::kSync:
        // Sync frame: ALL tones ON (known pattern) + pilot
        if (samplesInPhase_ < sps_) {
            for (int t = 0; t < bitsPerFrame_; t++) {
                double phase = kTones[t] * samplesInPhase_ / cfg_.sampleRate;
                output += toneSample(kTones[t], phase);
            }
        }
        samplesInPhase_++;
        if (samplesInPhase_ >= sps_ * cfg_.syncSymbols) {
            symbolIndex_ = 0;
            advancePhase();
        }
        break;

    case Phase::kData: {
        if (bitPos_ >= (int)bitstream_.size()) {
            advancePhase();
            break;
        }
        // Build one frame: mix tones for the next 4 bits
        if (samplesInPhase_ < sps_) {
            for (int t = 0; t < bitsPerFrame_ && (bitPos_ + t) < (int)bitstream_.size(); t++) {
                if (bitstream_[bitPos_ + t]) {
                    double phase = kTones[t] * samplesInPhase_ / cfg_.sampleRate;
                    output += toneSample(kTones[t], phase);
                }
            }
        }
        samplesInPhase_++;
        if (samplesInPhase_ >= sps_) {
            samplesInPhase_ = 0;
            bitPos_ += bitsPerFrame_;
        }
        break;
    }

    case Phase::kDone:
        if (onDone_) onDone_();
        return false;
    }

    hal::dacWriteFloat(output);
    return phase_ != Phase::kDone;
}

}} // namespace
