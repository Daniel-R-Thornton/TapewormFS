#include "modem_encoder.hpp"
#include "esp32_hal.hpp"
#include <cmath>
#include <cstring>

namespace tapefs { namespace firmware {

void ModemEncoder::outputSample(float toneFreq) {
    float sample = 0;

    if (toneFreq > 0) {
        // FSK tone
        double phase = 2.0 * M_PI * toneFreq * samplesInPhase_ / cfg_.sampleRate;
        sample = std::sin(phase);
    }

    // Add pilot
    sample += pilotSample();

    // Output via HAL — same call on ESP32 and in simulation
    hal::dacWriteFloat(sample);
}

float ModemEncoder::pilotSample() {
    float val = std::sin(2.0 * M_PI * pilotPhase_);
    pilotPhase_ += cfg_.pilotFreqHz / cfg_.sampleRate;
    if (pilotPhase_ >= 1.0) pilotPhase_ -= 1.0;
    return val * cfg_.pilotAmplitude;
}

void ModemEncoder::startEncoding(const std::vector<uint8_t>& data) {
    samplesPerSymbol_ = cfg_.sampleRate / cfg_.symbolsPerSec;
    guardSamples_ = samplesPerSymbol_ / 10; // ~10% guard
    pilotPhase_ = 0;
    encoding_ = true;
    phase_ = Phase::kPilot;
    samplesInPhase_ = 0;
    symbolIndex_ = 0;

    // Convert bytes to tone symbols
    symbols_.clear();
    for (auto byte : data) {
        int hi = (byte >> 5) & 0x07;
        symbols_.push_back(hi);
    }
}

bool ModemEncoder::generateSample() {
    if (!encoding_) return false;

    // Tone frequencies: 400, 600, 800, 1000, 1150, 1300, 1450, 1550 Hz
    const double tones[] = {400, 600, 800, 1000, 1150, 1300, 1450, 1550};

    switch (phase_) {
    case Phase::kPilot:
        // Leader: just pilot tone
        outputSample(0);
        samplesInPhase_++;
        if (samplesInPhase_ >= cfg_.sampleRate) { // 1 second leader
            phase_ = Phase::kSync;
            samplesInPhase_ = 0;
        }
        break;

    case Phase::kSync:
        outputSample(tones[0]); // sync = tone 0
        samplesInPhase_++;
        if (samplesInPhase_ >= samplesPerSymbol_ * cfg_.syncSymbols) {
            phase_ = Phase::kData;
            samplesInPhase_ = 0;
        }
        break;

    case Phase::kData:
        if (symbolIndex_ >= (int)symbols_.size()) {
            phase_ = Phase::kGuard;
            samplesInPhase_ = 0;
            break;
        }
        outputSample(tones[symbols_[symbolIndex_]]);
        samplesInPhase_++;
        if (samplesInPhase_ >= samplesPerSymbol_) {
            samplesInPhase_ = 0;
            symbolIndex_++;
        }
        break;

    case Phase::kGuard:
        outputSample(0); // silence + pilot
        samplesInPhase_++;
        if (samplesInPhase_ >= guardSamples_) {
            phase_ = Phase::kDone;
            encoding_ = false;
        }
        break;

    case Phase::kDone:
        encoding_ = false;
        return false;
    }

    return encoding_;
}

void ModemEncoder::encode(const std::vector<uint8_t>& data) {
    startEncoding(data);
    while (encoding_) {
        generateSample();
        // In real ESP32, this is driven by the timer ISR at sample rate.
        // In simulation, we just call it in a loop.
    }
}

}} // namespace tapefs::firmware
