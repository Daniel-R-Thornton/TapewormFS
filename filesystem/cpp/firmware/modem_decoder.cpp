#include "modem_decoder.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace tapefs { namespace firmware {

constexpr double ModemDecoder::kTones_[8];

int ModemDecoder::detectTone(const std::vector<float>& window) {
    double bestMag = 0;
    int bestTone = 0;
    int n = (int)window.size();

    for (int t = 0; t < 8; t++) {
        double sinCorr = 0, cosCorr = 0;
        for (int i = 0; i < n; i++) {
            double phase = 2.0 * M_PI * kTones_[t] * i / cfg_.sampleRate;
            sinCorr += window[i] * std::sin(phase);
            cosCorr += window[i] * std::cos(phase);
        }
        double mag = std::sqrt(sinCorr * sinCorr + cosCorr * cosCorr) / n;
        if (mag > bestMag) { bestMag = mag; bestTone = t; }
    }
    return bestTone;
}

void ModemDecoder::feedSample(float sample) {
    buf_.push_back(sample);

    // Track pilot zero-crossings
    if (lastSample_ < 0 && sample >= 0) zeroCrossCount_++;
    lastSample_ = sample;

    // Detect symbol when we have enough samples
    int sps = cfg_.sampleRate / cfg_.symbolsPerSec;
    if ((int)buf_.size() >= sps + guardSamples_) {
        auto window = std::vector<float>(buf_.begin(), buf_.begin() + sps);
        buf_.erase(buf_.begin(), buf_.begin() + sps + guardSamples_);

        int tone = detectTone(window);
        symbols_.push_back(tone);
        symbolCount_++;

        // Look for sync (4 consecutive tone-0 symbols)
        if (symbolCount_ >= 4) {
            int n = symbolCount_;
            if (symbols_[n-1] == 0 && symbols_[n-2] == 0 &&
                symbols_[n-3] == 0 && symbols_[n-4] == 0) {
                // Found sync — collect data symbols after it
                inFrame_ = true;
                syncCount_ = symbolCount_;
            }
        }

        // If in a frame, collect data tones
        if (inFrame_ && symbolCount_ > syncCount_ + 4) {
            // Convert collected symbols to bytes
            std::vector<uint8_t> bytes;
            for (int i = syncCount_ + 4; i < symbolCount_ - 4; i += 3) {
                uint8_t byte = 0;
                for (int j = 0; j < 3 && i + j < symbolCount_; j++) {
                    byte = (byte << 3) | (symbols_[i + j] & 0x07);
                }
                bytes.push_back(byte);
            }
            if (!bytes.empty() && onData_) {
                onData_(bytes);
            }
            decodedBytes_ = bytes;
            inFrame_ = false;
        }
    }
}

std::vector<uint8_t> ModemDecoder::decode(const std::vector<float>& samples) {
    buf_.clear();
    symbols_.clear();
    symbolCount_ = 0;
    inFrame_ = false;
    syncCount_ = 0;
    decodedBytes_.clear();
    zeroCrossCount_ = 0;

    for (auto s : samples) feedSample(s);

    return decodedBytes_;
}

}} // namespace tapefs::firmware
