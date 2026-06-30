#include "modem_decoder.hpp"
#include <cmath>

namespace tapefs { namespace firmware {

constexpr double ModemDecoder::kTones[4];

void ModemDecoder::reset() {
    buf_.clear();
    inFrame_ = false;
    frameReady_ = false;
    frameData_.clear();
    consecutiveSync_ = 0;
    frameSkip_ = 0;
    bitCollector_.clear();
    bitsCollected_ = 0;
    for (int i = 0; i < 4; i++) noiseFloor_[i] = 0;
    noiseCount_ = 0;
}

void ModemDecoder::startDecoding() { reset(); }

double ModemDecoder::detectEnergy(const float* samples, int n, int toneIdx) {
    double freq = kTones[toneIdx];
    double sinCorr = 0, cosCorr = 0;
    for (int i = 0; i < n; i++) {
        double phase = 2.0 * M_PI * freq * i / cfg_.sampleRate;
        sinCorr += samples[i] * std::sin(phase);
        cosCorr += samples[i] * std::cos(phase);
    }
    return (sinCorr * sinCorr + cosCorr * cosCorr) / (n * n);
}

void ModemDecoder::emitFrame() {
    frameData_.clear();
    uint8_t byte = 0;
    int bits = 0;
    for (auto b : bitCollector_) {
        byte = (byte << 1) | (b & 1);
        if (++bits >= 8) {
            frameData_.push_back(byte);
            byte = 0; bits = 0;
        }
    }
    if (bits > 0) frameData_.push_back(byte << (8 - bits));
    frameReady_ = true;
    if (onFrame_) onFrame_(frameData_);
}

void ModemDecoder::feedSample(float sample) {
    buf_.push_back(sample);
    if ((int)buf_.size() < sps_) return;

    auto window = buf_.data();
    buf_.erase(buf_.begin(), buf_.begin() + sps_);

    // Energy at each tone
    double energies[4], total = 0;
    for (int t = 0; t < 4; t++) {
        energies[t] = detectEnergy(window, sps_, t);
        total += energies[t];
    }

    // Quick noise floor estimation (first 5 frames, all pilot)
    if (noiseCount_ < 5) {
        for (int t = 0; t < 4; t++)
            noiseFloor_[t] = (noiseFloor_[t] * noiseCount_ + energies[t]) / (noiseCount_ + 1);
        noiseCount_++;
        return;  // don't try to sync during noise calibration
    }

    // Slow adaptation after calibration
    for (int t = 0; t < 4; t++)
        noiseFloor_[t] = noiseFloor_[t] * 0.99 + energies[t] * 0.01;

    // Sync: all 4 tones strong → avg energy >> noise floor
    double avg = total / 4;
    double noise = 0;
    for (int t = 0; t < 4; t++) noise += noiseFloor_[t];
    noise /= 4;

    bool strong = (noise > 0.0001) && (avg > noise * 4.0);

    if (strong) {
        consecutiveSync_++;
    } else {
        consecutiveSync_ = 0;
        frameSkip_ = 0;
    }

    // Enter data mode after 2 sync frames
    if (consecutiveSync_ >= 2 && !inFrame_) {
        inFrame_ = true;
        bitCollector_.clear();
        bitsCollected_ = 0;
        frameSkip_ = 1;  // skip sync tail before collecting data
        return;
    }

    // Decode bits during frame
    if (inFrame_) {
        if (frameSkip_ > 0) {
            frameSkip_--;
            return;
        }

        for (int t = 0; t < 4; t++) {
            bitCollector_.push_back(energies[t] > noiseFloor_[t] * 2.0 ? 1 : 0);
            bitsCollected_++;
        }

        // End of frame: energy drops back to noise
        if (avg < noise * 1.5 && bitsCollected_ > 48) {
            emitFrame();
            inFrame_ = false;
        }
        if (bitsCollected_ > 800) {
            emitFrame();
            inFrame_ = false;
        }
    }
}

std::vector<uint8_t> ModemDecoder::takeFrame() {
    frameReady_ = false;
    return std::move(frameData_);
}

}} // namespace
