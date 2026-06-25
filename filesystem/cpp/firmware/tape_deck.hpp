#pragma once
#include "motor.hpp"
#include "tape_head.hpp"
#include "tape_medium.hpp"
#include <cstdint>
#include <memory>

namespace tapefs { namespace firmware {

struct TapeDeckConfig {
    int    baudRate    = 200;
    int    blockSize   = 1024;
    double overheadPct = 1.3;
};

class TapeDeck {
public:
    explicit TapeDeck(TapeDeckConfig cfg = {}, TapeMedium* medium = nullptr);

    void seekToBlock(int blockNo);
    void rewind();
    void stop();
    void tick();

    bool isBusy() const { return state_ != 0; }
    int  currentBlock() const { return head_.currentBlock(); }
    double positionMM() const { return head_.positionMM(); }
    double blockTimeMs() const;

    Motor&    motor()   { return motor_; }
    TapeHead& head()    { return head_; }

private:
    int state_ = 0, targetBlock_ = 0;
    double seekDurationMs_ = 0;
    int64_t seekStartUs_ = 0;
    Motor    motor_;
    TapeHead head_;
    TapeMedium* medium_ = nullptr;
};

}}
