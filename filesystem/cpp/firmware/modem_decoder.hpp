#pragma once
/**
 * ModemDecoder — FSK decoder driven by timer ISR.
 *
 * Real ESP32 usage:
 *   decoder.startDecoding();
 *   // timer ISR at 3200 Hz: decoder.feedSample(hal::adcReadFloat())
 *   // when frameReady(), take the data
 */

#include <cstdint>
#include <vector>
#include <functional>

namespace tapefs { namespace firmware {

class ModemDecoder {
public:
    ModemDecoder() : ModemDecoder(Config{}) {}
    using FrameCallback = std::function<void(const std::vector<uint8_t>&)>;

    struct Config {
        int    sampleRate    = 3200;
        int    symbolsPerSec = 50;
        double pilotFreqHz   = 62.5;
    };

    explicit ModemDecoder(Config cfg) : cfg_(cfg) {}

    /// Start listening for frames.
    void startDecoding();

    /// Call from timer ISR: feed one ADC sample.
    void feedSample(float sample);

    bool isFrameReady() const { return frameReady_; }
    std::vector<uint8_t> takeFrame();

    void onFrame(FrameCallback cb) { onFrame_ = std::move(cb); }

private:
    Config cfg_;
    FrameCallback onFrame_;

    std::vector<float> buf_;
    int sps_ = 64;
    int guard_ = 6;

    std::vector<int> symbols_;
    int consecutiveZeros_ = 0;
    bool inFrame_ = false;
    int dataSymbols_ = 0;
    bool frameReady_ = false;
    std::vector<uint8_t> frameData_;

    int detectTone(const float* samples, int n);
    void emitFrame();
    void reset();
};

}} // namespace
