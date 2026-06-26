#include "modem_decoder.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace tapefs { namespace firmware {

static constexpr double kTones[8] = {400, 600, 800, 1000, 1150, 1300, 1450, 1550};

void ModemDecoder::reset() {
    buf_.clear();
    symbols_.clear();
    consecutiveZeros_ = 0;
    inFrame_ = false;
    dataSymbols_ = 0;
    frameReady_ = false;
    frameData_.clear();
}

void ModemDecoder::startDecoding() {
    reset();
}

int ModemDecoder::detectTone(const float* samples, int n) {
    double bestMag = 0;
    int bestTone = 0;

    for (int t = 0; t < 8; t++) {
        double sinCorr = 0, cosCorr = 0;
        for (int i = 0; i < n; i++) {
            double phase = 2.0 * M_PI * kTones[t] * i / cfg_.sampleRate;
            sinCorr += samples[i] * std::sin(phase);
            cosCorr += samples[i] * std::cos(phase);
        }
        double mag = (sinCorr * sinCorr + cosCorr * cosCorr) / (n * n);
        if (mag > bestMag) { bestMag = mag; bestTone = t; }
    }
    return bestTone;
}

void ModemDecoder::emitFrame() {
    // Convert collected symbols to bytes
    frameData_.clear();
    for (size_t i = 0; i + 1 < symbols_.size(); i += 2) {
        uint8_t byte = (symbols_[i] << 5) | (symbols_[i+1] & 0x07);
        frameData_.push_back(byte);
    }
    frameReady_ = true;
    if (onFrame_) onFrame_(frameData_);
}

void ModemDecoder::feedSample(float sample) {
    buf_.push_back(sample);

    if ((int)buf_.size() < sps_ + guard_) return;

    // We have a full symbol window
    auto window = buf_.data();
    int tone = detectTone(window, sps_);
    buf_.erase(buf_.begin(), buf_.begin() + sps_ + guard_);

    symbols_.push_back(tone);

    // Track consecutive zeros (sync pattern)
    if (tone == 0) {
        consecutiveZeros_++;
    } else {
        consecutiveZeros_ = 0;
    }

    // 4 consecutive zeros = sync found
    if (consecutiveZeros_ >= 4 && !inFrame_) {
        inFrame_ = true;
        dataSymbols_ = 0;
        symbols_.clear();
        return;
    }

    // Collect data symbols
    if (inFrame_) {
        dataSymbols_++;
        // End of frame: 2 consecutive zeros or 100 symbols max
        if (dataSymbols_ > 100 || (dataSymbols_ > 2 && tone == 0)) {
            if (!symbols_.empty()) emitFrame();
            inFrame_ = false;
            dataSymbols_ = 0;
            consecutiveZeros_ = (tone == 0) ? 1 : 0;
        }
    }
}

std::vector<uint8_t> ModemDecoder::takeFrame() {
    frameReady_ = false;
    return std::move(frameData_);
}

}} // namespace
