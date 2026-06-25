#pragma once
/**
 * FSK Modem — same code runs on ESP32 and in simulation.
 *
 * The only hardware it touches is:
 *   hal::dacWriteFloat(sample)  — output audio
 *   hal::adcReadFloat(pin)      — input audio
 *   hal::timerStart()           — sample timing
 *
 * Swap the HAL implementation and it runs on real hardware.
 */

#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>

namespace tapefs { namespace firmware {

class ModemEncoder {
public:
    struct Config {
        int   sampleRate      = 3200;
        int   symbolsPerSec   = 50;
        double pilotFreqHz    = 62.5;
        double pilotAmplitude = 0.15;
        int   syncSymbols     = 4;
    };

    ModemEncoder() : ModemEncoder(Config{}) {}
    explicit ModemEncoder(Config cfg) : cfg_(cfg) {}

    /// Encode bytes → audio samples via hal::dacWriteFloat().
    /// This is the function that runs on the ESP32 timer ISR.
    void encode(const std::vector<uint8_t>& data);

    /// Generate one sample (call from timer ISR at sample rate).
    /// Returns false until encoding is complete.
    bool generateSample();

    /// Start encoding (call this, then let the timer call generateSample)
    void startEncoding(const std::vector<uint8_t>& data);

    bool isEncoding() const { return encoding_; }

private:
    Config cfg_;
    bool encoding_ = false;

    // Encoder state machine
    enum class Phase { kPilot, kSync, kData, kGuard, kDone };
    Phase phase_ = Phase::kDone;
    int samplesInPhase_ = 0;
    int samplesPerSymbol_;
    int guardSamples_;

    // Bitstream
    std::vector<int> symbols_;
    int symbolIndex_ = 0;

    // Pilot
    double pilotPhase_ = 0;

    float pilotSample();
    void outputSample(float toneFreq = 0);
    int bitsPerSymbol() const { return 3; }
};

}} // namespace tapefs::firmware
