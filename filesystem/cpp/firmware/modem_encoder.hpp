#pragma once
/**
 * ModemEncoder — Multi-tone encoder.
 *
 * Transmits 4 bits per symbol by mixing 4 frequencies simultaneously.
 * Each frequency is ON (bit=1) or OFF (bit=0).
 * Frame structure: pilot leader → sync → data → done.
 */

#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>

namespace tapefs { namespace firmware {

class ModemEncoder {
public:
    using DoneCallback = std::function<void()>;

    struct Config {
        int    sampleRate     = 3200;
        int    symbolsPerSec  = 25;      // 25 symbols/s, 128 samples each
        double pilotFreqHz    = 62.5;
        double pilotAmplitude = 0.125;
        int    syncSymbols    = 2;       // 2 sync frames
        int    bitsPerFrame   = 4;       // 4 bits per frame = 1 nybble
    };

    // 4 tone frequencies (Hz) — each carries 1 bit
    static constexpr double kTones[4] = {500, 700, 900, 1100};

    ModemEncoder() : ModemEncoder(Config{}) {}
    explicit ModemEncoder(Config cfg) : cfg_(cfg) {}

    void startEncoding(const std::vector<uint8_t>& data);
    bool generateSample();  // call from timer ISR
    bool isEncoding() const { return phase_ != Phase::kDone; }
    void onDone(DoneCallback cb) { onDone_ = std::move(cb); }

private:
    Config cfg_;
    DoneCallback onDone_;

    int sps_ = 128;  // samples per symbol
    int bitsPerFrame_ = 4;

    enum class Phase { kLeader, kSync, kData, kDone };
    Phase phase_ = Phase::kDone;
    int samplesInPhase_ = 0;
    double pilotPhase_ = 0;
    int symbolIndex_ = 0;

    // Bitstream to transmit
    std::vector<uint8_t> bitstream_;
    int bitPos_ = 0;

    float pilotSample();
    float toneSample(double freqHz, double phase);
    void  advancePhase();
};

}} // namespace
