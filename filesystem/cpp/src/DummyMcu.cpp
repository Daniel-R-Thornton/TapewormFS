#include "tapefs/DummyMcu.hpp"
#include <thread>
#include <algorithm>
#include <cstring>

namespace tapefs {

bool DummyMcu::rawWrite(const ByteBuffer& block) {
    if (errorNoTape_ || errorWriteProtect_) return false;
    if (std::this_thread::sleep_for(writeDelay_), false) {} // delay

    if (position_ >= tape_.size()) {
        tape_.push_back(block);
    } else {
        tape_[position_] = block;
    }
    position_++;
    return true;
}

std::optional<ByteBuffer> DummyMcu::rawRead() {
    if (position_ >= tape_.size()) return std::nullopt;
    if (std::this_thread::sleep_for(readDelay_), false) {} // delay

    auto result = tape_[position_];

    // Inject CRC error if configured
    if (errorCrcRate_ > 0) {
        opCount_++;
        if (opCount_ % errorCrcRate_ == 0 && !result.empty()) {
            result.back() ^= 0xFF; // flip last byte (CRC)
        }
    }

    position_++;
    return result;
}

bool DummyMcu::rawSeek(BlockNumber blockNo) {
    if (errorNoTape_) return false;

    auto distance = static_cast<size_t>(
        std::abs(static_cast<int>(blockNo) - static_cast<int>(position_)));
    auto delay = seekDelay_ * static_cast<int>(distance);
    if (delay.count() > 0) {
        std::this_thread::sleep_for(delay);
    }

    position_ = std::min<size_t>(blockNo, tape_.size());
    return true;
}

} // namespace tapefs
