#pragma once

#include "tapefs/Types.hpp"
#include <vector>
#include <optional>
#include <chrono>

namespace tapefs {

/// Software-emulated ESP32 firmware for testing.
/// Stores blocks in memory, simulates latency and errors.
class DummyMcu {
public:
    DummyMcu() = default;

    // ---- Raw block callbacks (for Filesystem) --------------------- //

    bool rawWrite(const ByteBuffer& block);
    std::optional<ByteBuffer> rawRead();
    bool rawSeek(BlockNumber blockNo);

    // ---- Inspection (for tests) ----------------------------------- //

    const auto& tape() const { return tape_; }
    size_t tapeSize() const { return tape_.size(); }

    // ---- Error injection ------------------------------------------ //

    void setErrorCrcRate(int everyN) { errorCrcRate_ = everyN; }
    void setErrorNoTape(bool enabled) { errorNoTape_ = enabled; }
    void setErrorWriteProtect(bool enabled) { errorWriteProtect_ = enabled; }

    // ---- Configuration -------------------------------------------- //

    void setDelay(std::chrono::milliseconds writeDelay,
                  std::chrono::milliseconds readDelay,
                  std::chrono::milliseconds seekDelay) {
        writeDelay_ = writeDelay;
        readDelay_  = readDelay;
        seekDelay_  = seekDelay;
    }

private:
    std::vector<ByteBuffer> tape_;
    size_t position_ = 0;

    int  errorCrcRate_      = 0;
    bool errorNoTape_       = false;
    bool errorWriteProtect_ = false;

    std::chrono::milliseconds writeDelay_{10};
    std::chrono::milliseconds readDelay_{10};
    std::chrono::milliseconds seekDelay_{5};
    int  opCount_ = 0;
};

} // namespace tapefs
