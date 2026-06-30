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

// Returns tone index 0-7.  Also sets outMag to the correlation magnitude.
// Real FSK tones give mag ~0.25, pilot noise gives mag < 0.02.
static int detectToneMag(const float* samples, int n, double* outMag) {
    double bestMag = 0;
    int bestTone = 0;
    for (int t = 0; t < 8; t++) {
        double sinCorr = 0, cosCorr = 0;
        for (int i = 0; i < n; i++) {
            double phase = 2.0 * M_PI * kTones[t] * i / 3200.0;
            sinCorr += samples[i] * std::sin(phase);
            cosCorr += samples[i] * std::cos(phase);
        }
        double mag = (sinCorr * sinCorr + cosCorr * cosCorr) / (n * n);
        if (mag > bestMag) { bestMag = mag; bestTone = t; }
    }
    *outMag = bestMag;
    return bestTone;
}

void ModemDecoder::emitFrame() {
    frameData_.clear();
    // Each symbol is 3 bits, pack into bytes (8 bits each)
    uint8_t buf = 0;
    int bits = 0;
    for (int t : symbols_) {
        buf = (buf << 3) | (t & 0x07);
        bits += 3;
        if (bits >= 8) {
            bits -= 8;
            frameData_.push_back(buf >> bits);
            buf &= (1 << bits) - 1;
        }
    }
    if (bits > 0) {
        frameData_.push_back(buf << (8 - bits));
    }
    frameReady_ = true;
    if (onFrame_) onFrame_(frameData_);
}


void ModemDecoder::feedSample(float sample) {
    buf_.push_back(sample);
    if ((int)buf_.size() < sps_ + guard_) return;

    auto window = buf_.data();
    double mag = 0;
    int tone = detectToneMag(window, sps_, &mag);
    buf_.erase(buf_.begin(), buf_.begin() + sps_ + guard_);

    // Energy distinguishes real FSK signal from pilot noise
    double energy = 0;
    for (int i = 0; i < sps_; i++) energy += window[i] * window[i];
    energy /= sps_;
    bool realSignal = energy > 0.02;  // pilot:~0.011, FSK:~0.36

    if (realSignal) {
        if (tone == 0) consecutiveZeros_++; else consecutiveZeros_ = 0;
    } else {
        consecutiveZeros_ = 0;
        // End of frame when signal drops after collecting data
        if (inFrame_ && dataSymbols_ > 5) {
            emitFrame();
            inFrame_ = false;
            dataSymbols_ = 0;
            return;
        }
    }

    // Sync: 4 consecutive real tone-0 symbols
    if (consecutiveZeros_ >= 4 && !inFrame_) {
        inFrame_ = true;
        dataSymbols_ = 0;
        symbols_.clear();
        return;
    }

    // Collect data symbols during frame
    if (inFrame_ && realSignal) {
        symbols_.push_back(tone);
        dataSymbols_++;
    }

    // Safety limit
    if (inFrame_ && dataSymbols_ > 60) {
        emitFrame();
        inFrame_ = false;
        dataSymbols_ = 0;
    }
}

std::vector<uint8_t> ModemDecoder::takeFrame() {
    frameReady_ = false;
    return std::move(frameData_);
}

}} // namespace
