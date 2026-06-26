#pragma once
/**
 * ModemEncoder — FSK encoder driven by timer ISR.
 *
 * Real ESP32 usage:
 *   encoder.startEncoding(myData);
 *   // timer ISR at 3200 Hz calls encoder.generateSample()
 *   // which calls hal::dacWriteFloat() internally
 */

#include <cstdint>
#include <vector>
#include <functional>

namespace tapefs { namespace firmware {

class ModemEncoder {
public:
    ModemEncoder() : ModemEncoder(Config{}) {}
    using DoneCallback = std::function<void()>;

    struct Config {
        int    sampleRate     = 3200;
        int    symbolsPerSec  = 50;
        double pilotFreqHz    = 62.5;
        double pilotAmplitude = 0.15;
        int    syncSymbols    = 4;
    };

    explicit ModemEncoder(Config cfg) : cfg_(cfg) {}

    /// Set up encoding state.  Start the timer after this.
    void startEncoding(const std::vector<uint8_t>& data);

    /// Call from timer ISR.  Generates one sample → DAC.
    /// Returns true while encoding is active.
    bool generateSample();

    bool isEncoding() const { return phase_ != Phase::kDone; }

    void onDone(DoneCallback cb) { onDone_ = std::move(cb); }

private:
    Config cfg_;
    DoneCallback onDone_;

    enum class Phase { kLeader, kSync, kData, kGuard, kDone };
    Phase phase_ = Phase::kDone;
    int samplesInPhase_ = 0;
    int sps_ = 64;
    int guard_ = 6;
    double pilotPhase_ = 0;
    int symbolIndex_ = 0;

    std::vector<int> symbols_;

    float pilotSample();
    float toneSample(double freqHz);
    void  advancePhase();
};

}} // namespace
