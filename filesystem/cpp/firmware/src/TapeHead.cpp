#include "tapefs/firmware/TapeHead.hpp"
#include <algorithm>
#include <cmath>

namespace tapefs { namespace firmware {


int TapeHead::currentBlock() const {
    if (locations_.empty()) return 0;
    // Find the nearest block to current position
    int best = 0;
    double bestDist = std::abs(positionMM_ - locations_[0].positionMM);
    for (size_t i = 1; i < locations_.size(); i++) {
        double d = std::abs(positionMM_ - locations_[i].positionMM);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    return locations_[best].blockNumber;
}

void TapeHead::seekToBlock(int blockNo) {
    // Calculate target position from block number
    double targetMM = cfg_.leaderLengthMM + blockNo * cfg_.blockSpacingMM;
    targetMM = std::clamp(targetMM, 0.0, cfg_.totalTapeMM);
    // The motor controller handles the actual movement;
    // we just report where we want to be.
}

void TapeHead::tick(double dt) {
    if (!motor_) return;
    positionMM_ = motor_->currentPositionMM();
}

bool TapeHead::isOverBlock() const {
    for (const auto& loc : locations_) {
        double halfBlock = cfg_.blockLengthMM / 2.0;
        if (std::abs(positionMM_ - loc.positionMM) < halfBlock) {
            return loc.written;
        }
    }
    return false;
}

int TapeHead::readBlockAtHead(std::vector<uint8_t>& outData) const {
    for (size_t i = 0; i < locations_.size(); i++) {
        const auto& loc = locations_[i];
        double halfBlock = cfg_.blockLengthMM / 2.0;
        if (std::abs(positionMM_ - loc.positionMM) < halfBlock) {
            if (!loc.written) return -1;
            if (i < blockData_.size()) {
                outData = blockData_[i].bytes;
                return loc.blockNumber;
            }
        }
    }
    return -1;
}

bool TapeHead::writeBlock(int blockNo, const std::vector<uint8_t>& data) {
    // Record this block at the current position
    BlockLocation loc;
    loc.blockNumber = blockNo;
    loc.positionMM  = positionMM_;
    loc.written     = true;
    locations_.push_back(loc);

    BlockData bd;
    bd.bytes = data;
    blockData_.push_back(bd);
    return true;
}

}} // namespace tapefs::firmware
