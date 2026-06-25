#pragma once

#include <cstdint>
#include <vector>
#include <functional>

namespace tapefs { namespace firmware {

class ModemEncoder {
public:
    using SampleCallback = std::function<void(float)>;

    struct Config {
        int    sampleRate      = 3200;
        int    symbolsPerSec   = 50;
        int    bitsPerSymbol   = 3;
        double pilotFreqHz     = 62.5;
        double pilotAmplitude  = 0.15;
        int    syncSymbols     = 4;
    };

    ModemEncoder() : ModemEncoder(Config{}) {}
    explicit ModemEncoder(Config cfg) : cfg_(cfg) {}

    void encode(const std::vector<uint8_t>& data);
    void setSampleCallback(SampleCallback cb) { onSample_ = std::move(cb); }
    const std::vector<float>& output() const { return output_; }

private:
    Config  cfg_;
    SampleCallback onSample_;
    std::vector<float> output_;
    double  pilotPhase_ = 0;
    int     symbolCount_ = 0;
    void emitSample(float s);
    float pilotSample(bool invert = false);
    void  fskSymbol(int toneValue);
    void  guard();
};

class ModemDecoder {
public:
    using SymbolCallback = std::function<void(int)>;

    struct Config {
        int    sampleRate     = 3200;
        int    symbolsPerSec  = 50;
        int    bitsPerSymbol  = 3;
        double pilotFreqHz    = 62.5;
    };

    ModemDecoder() : ModemDecoder(Config{}) {}
    explicit ModemDecoder(Config cfg) : cfg_(cfg) {}

    void feedSample(float sample);
    void setSymbolCallback(SymbolCallback cb) { onSymbol_ = std::move(cb); }
    bool frameReady() const { return frameReady_; }
    std::vector<uint8_t> takeFrame();

private:
    Config  cfg_;
    SymbolCallback onSymbol_;
    std::vector<float> symbolBuf_;
    int sampleCount_ = 0;
    int samplesPerSymbol_ = 0;
    std::vector<int> symbols_;
    bool frameReady_ = false;
    double pilotPhase_ = 0;
    double lastPilot_  = 0;
    int    zeroCrossCount_ = 0;
};

}}
