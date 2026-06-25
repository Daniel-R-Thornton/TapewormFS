#pragma once
/**
 * FSK Demodulator — same code runs on ESP32 and in simulation.
 *
 * Reads audio via hal::adcReadFloat().
 * Outputs decoded bytes via callback.
 */

#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>

namespace tapefs { namespace firmware {

class ModemDecoder {
public:
    using DataCallback = std::function<void(const std::vector<uint8_t>&)>;

    struct Config {
        int   sampleRate      = 3200;
        int   symbolsPerSec   = 50;
        double pilotFreqHz    = 62.5;
    };

    ModemDecoder() : ModemDecoder(Config{}) {}
    explicit ModemDecoder(Config cfg) : cfg_(cfg) {}

    /// Feed one sample (call this from the ADC timer ISR).
    void feedSample(float sample);

    /// Set a callback for decoded data frames.
    void setDataCallback(DataCallback cb) { onData_ = std::move(cb); }

    /// Decode a buffer of samples (for file-based use).
    /// Returns decoded bytes.
    std::vector<uint8_t> decode(const std::vector<float>& samples);

private:
    Config cfg_;
    DataCallback onData_;
    std::vector<float> buf_;
    int samplesPerSymbol_ = 64;
    int guardSamples_ = 6;

    // Decoder state
    std::vector<int> symbols_;
    int symbolCount_ = 0;
    bool inFrame_ = false;
    int syncCount_ = 0;

    // Tone detection
    int detectTone(const std::vector<float>& window);

    // Pilot tracking
    double pilotPhase_ = 0;
    double lastSample_ = 0;
    int zeroCrossCount_ = 0;

    std::vector<uint8_t> decodedBytes_;

    // Tone frequencies (same as encoder)
    static constexpr double kTones_[8] = {400, 600, 800, 1000, 1150, 1300, 1450, 1550};
};

}} // namespace tapefs::firmware
