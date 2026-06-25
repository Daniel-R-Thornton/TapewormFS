#pragma once

#include "Motor.hpp"
#include "TapeHead.hpp"
#include "Modem.hpp"
#include <cstdint>
#include <vector>
#include <functional>
#include <optional>
#include <memory>

namespace tapefs { namespace firmware {

class TapeDeck {
public:
    enum class State { kIdle, kMotorStart, kSeeking, kReading, kWriting, kRewinding, kError };

    struct Config {
        Motor::Config    motor;
        TapeHead::Config head;
        ModemEncoder::Config encoder;
        ModemDecoder::Config decoder;
    };

    TapeDeck() : TapeDeck(Config{}) {}
    explicit TapeDeck(Config cfg);

    void tick();

    void seekToBlock(int blockNo);
    void rewind();
    void startWrite();
    void writeBuffer(const std::vector<uint8_t>& blockData);
    void flush();
    std::optional<std::vector<uint8_t>> readNextBlock();
    void stop();

    State state() const { return state_; }
    int   currentBlock() const { return head_->currentBlock(); }
    double positionMM() const { return head_->positionMM(); }
    bool  isBusy() const { return state_ != State::kIdle && state_ != State::kError; }
    bool  isAtBOT() const { return head_->positionMM() < 0.1; }
    bool  isAtEOT() const { return head_->positionMM() >= 89999.0; }

    size_t blockCount() const { return blocks_.size(); }

private:
    Config  cfg_;
    State   state_ = State::kIdle;
    int     targetBlock_ = 0;

    std::unique_ptr<Motor>    motor_;
    std::unique_ptr<TapeHead>  head_;
    std::unique_ptr<ModemEncoder> encoder_;
    std::unique_ptr<ModemDecoder> decoder_;

    std::vector<std::vector<uint8_t>> writeBuffer_;
    std::vector<std::vector<uint8_t>> blocks_;

    void enterState(State s);
    bool checkArrived();
};

}}
