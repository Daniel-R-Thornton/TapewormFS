#pragma once
/**
 * ModemDecoder — Multi-tone decoder with adaptive noise floor.
 *
 * Each symbol window contains 4 frequencies mixed together.
 * Energy at each frequency → compare to noise floor → bit decision.
 * 4 bits per frame → pack into bytes.
 * Adaptive noise floor tracks the background level.
 */

#include <cstdint>
#include <vector>
#include <functional>

namespace tapefs { namespace firmware {

class ModemDecoder {
public:
    using FrameCallback = std::function<void(const std::vector<uint8_t>&)>;

    struct Config {
        int sampleRate     = 3200;
        int symbolsPerSec  = 25;
        int bitsPerFrame   = 4;
    };

    static constexpr double kTones[4] = {500, 700, 900, 1100};

    ModemDecoder() : ModemDecoder(Config{}) {}
    explicit ModemDecoder(Config cfg) : cfg_(cfg) {}

    void startDecoding();
    void feedSample(float sample);
    bool isFrameReady() const { return frameReady_; }
    std::vector<uint8_t> takeFrame();
    void onFrame(FrameCallback cb) { onFrame_ = std::move(cb); }

private:
    Config cfg_;
    FrameCallback onFrame_;
    int sps_ = 128;

    std::vector<float> buf_;
    bool inFrame_ = false;
    bool frameReady_ = false;
    std::vector<uint8_t> frameData_;

    int consecutiveSync_ = 0;
    int frameSkip_ = 0;  // frames to skip after sync
    std::vector<uint8_t> bitCollector_;
    int bitsCollected_ = 0;

    // Adaptive noise floor (per frequency band)
    double noiseFloor_[4] = {0, 0, 0, 0};
    int noiseCount_ = 0;

    double detectEnergy(const float* samples, int n, int toneIdx);
    void emitFrame();
    void reset();
};

}} // namespace
