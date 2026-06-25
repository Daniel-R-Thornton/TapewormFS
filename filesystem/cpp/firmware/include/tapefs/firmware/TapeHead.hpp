#pragma once

#include "Motor.hpp"
#include <cstdint>
#include <vector>

namespace tapefs { namespace firmware {

class TapeHead {
public:
    struct Config {
        double blockSpacingMM = 12.0;
        double blockLengthMM  = 8.0;
        double leaderLengthMM = 300.0;
        double totalTapeMM    = 90000.0;
    };

    TapeHead() : TapeHead(Config{}) {}
    explicit TapeHead(Config cfg) : cfg_(cfg) {}

    void attachMotor(Motor* motor) { motor_ = motor; }
    int  currentBlock() const;
    double positionMM() const { return positionMM_; }
    void seekToBlock(int blockNo);
    void tick(double dtSeconds);
    int  readBlockAtHead(std::vector<uint8_t>& outData) const;
    bool isOverBlock() const;
    bool writeBlock(int blockNo, const std::vector<uint8_t>& data);

    struct BlockLocation {
        int      blockNumber;
        double   positionMM;
        bool     written = false;
    };
    const auto& locations() const { return locations_; }

private:
    Config  cfg_;
    Motor*  motor_ = nullptr;
    double  positionMM_ = 0;
    std::vector<BlockLocation> locations_;
    struct BlockData { std::vector<uint8_t> bytes; };
    std::vector<BlockData> blockData_;
};

}}
