#include "tapefs/firmware/Modem.hpp"
#include <cmath>
#include <algorithm>

namespace tapefs { namespace firmware {

// ================================================================== //
//  ModemEncoder
// ================================================================== //


void ModemEncoder::emitSample(float s) {
    output_.push_back(s);
    if (onSample_) onSample_(s);
}

float ModemEncoder::pilotSample(bool invert) {
    float val = std::sin(2.0 * M_PI * pilotPhase_) * cfg_.pilotAmplitude;
    pilotPhase_ += cfg_.pilotFreqHz / cfg_.sampleRate;
    if (pilotPhase_ >= 1.0) pilotPhase_ -= 1.0;
    return invert ? -val : val;
}

void ModemEncoder::fskSymbol(int toneValue) {
    int sps = cfg_.sampleRate / cfg_.symbolsPerSec;
    double freq = 400.0 + toneValue * 200.0; // tones at 400, 600, 800... Hz
    for (int i = 0; i < sps; i++) {
        double t = static_cast<double>(i) / cfg_.sampleRate;
        float sample = std::sin(2.0 * M_PI * freq * t);
        sample += pilotSample();
        emitSample(sample);
    }
    symbolCount_++;
}

void ModemEncoder::guard() {
    for (int i = 0; i < 6; i++) emitSample(pilotSample());
}

void ModemEncoder::encode(const std::vector<uint8_t>& data) {
    pilotPhase_ = 0;
    symbolCount_ = 0;
    output_.clear();

    // Sync preamble
    for (int i = 0; i < cfg_.syncSymbols; i++) {
        fskSymbol(0);  // tone 0 = sync
        guard();
    }

    // Data
    for (auto byte : data) {
        int hi = (byte >> 5) & 0x07;       // top 3 bits → tone 0-7
        int lo = byte & 0x1F;               // bottom 5 bits
        fskSymbol(hi);
        guard();
        // For simplicity, only send top 3 bits per symbol.
        // Full implementation would pack properly.
    }

    // Final guard
    for (int i = 0; i < 6; i++) emitSample(pilotSample());
}

// ================================================================== //
//  ModemDecoder
// ================================================================== //


void ModemDecoder::feedSample(float sample) {
    symbolBuf_.push_back(sample);
    sampleCount_++;

    // Track pilot zero-crossings for timing recovery
    if (lastPilot_ < 0 && sample >= 0) zeroCrossCount_++;
    lastPilot_ = sample;

    // When we have enough samples for a symbol, detect the tone
    if (static_cast<int>(symbolBuf_.size()) >= samplesPerSymbol_) {
        // Simple energy detection: find strongest frequency bin
        // (correlation-based, same as Python ToneDetector)
        const double freqs[] = {400, 600, 800, 1000, 1150, 1300, 1450, 1550};
        double bestMag = 0;
        int bestTone = 0;

        for (int tone = 0; tone < 8; tone++) {
            double sinCorr = 0, cosCorr = 0;
            for (int i = 0; i < samplesPerSymbol_; i++) {
                double phase = 2.0 * M_PI * freqs[tone] * i / cfg_.sampleRate;
                sinCorr += symbolBuf_[i] * std::sin(phase);
                cosCorr += symbolBuf_[i] * std::cos(phase);
            }
            double mag = std::sqrt(sinCorr * sinCorr + cosCorr * cosCorr)
                         / samplesPerSymbol_;
            if (mag > bestMag) { bestMag = mag; bestTone = tone; }
        }

        symbols_.push_back(bestTone);
        if (onSymbol_) onSymbol_(bestTone);
        symbolBuf_.clear();

        // Check for sync: 4 consecutive tone-0 symbols means start of frame
        int n = static_cast<int>(symbols_.size());
        if (n >= 4 && symbols_[n-1] == 0 && symbols_[n-2] == 0
                   && symbols_[n-3] == 0 && symbols_[n-4] == 0) {
            // (Frame ready would be signalled here in real impl)
        }
    }
}

std::vector<uint8_t> ModemDecoder::takeFrame() {
    frameReady_ = false;
    return {};
}

}} // namespace tapefs::firmware
