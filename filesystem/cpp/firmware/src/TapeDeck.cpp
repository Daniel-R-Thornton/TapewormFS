#include "tapefs/firmware/TapeDeck.hpp"
#include <algorithm>
#include <thread>
#include <chrono>

namespace tapefs { namespace firmware {

TapeDeck::TapeDeck(Config cfg) : cfg_(cfg) {
    motor_   = std::make_unique<Motor>(cfg.motor);
    head_    = std::make_unique<TapeHead>(cfg.head);
    encoder_ = std::make_unique<ModemEncoder>(cfg.encoder);
    decoder_ = std::make_unique<ModemDecoder>(cfg.decoder);
    head_->attachMotor(motor_.get());
}

void TapeDeck::tick() {
    motor_->tick(0.01);
    head_->tick(0.01);

    switch (state_) {
    case State::kSeeking:
        if (checkArrived()) {
            motor_->stop();
            enterState(State::kIdle);
        }
        break;
    case State::kRewinding:
        if (isAtBOT()) {
            motor_->stop();
            enterState(State::kIdle);
        }
        break;
    default: break;
    }
}

void TapeDeck::seekToBlock(int blockNo) {
    targetBlock_ = blockNo;
    head_->seekToBlock(blockNo);
    int current = head_->currentBlock();
    if (blockNo > current) motor_->fastForward();
    else if (blockNo < current) motor_->rewind();
    else return;
    enterState(State::kSeeking);
}

void TapeDeck::rewind() {
    motor_->rewind();
    enterState(State::kRewinding);
}

void TapeDeck::writeBuffer(const std::vector<uint8_t>& data) {
    writeBuffer_.push_back(data);
}

void TapeDeck::flush() {
    for (const auto& data : writeBuffer_) {
        int blockNo = static_cast<int>(blocks_.size());
        blocks_.push_back(data);
        head_->writeBlock(blockNo, data);
        double bits = data.size() * 8;
        double timeMs = (bits / 200.0) * 1300;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(timeMs)));
    }
    writeBuffer_.clear();
}

std::optional<std::vector<uint8_t>> TapeDeck::readNextBlock() {
    int blockNo = head_->currentBlock();
    int next = blockNo + 1;
    if (next >= static_cast<int>(blocks_.size()))
        return std::nullopt;
    double bits = blocks_[next].size() * 8;
    double timeMs = (bits / 200.0) * 1300;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<int>(timeMs)));
    return blocks_[next];
}

void TapeDeck::stop() {
    motor_->stop();
    enterState(State::kIdle);
}

void TapeDeck::enterState(State s) { state_ = s; }

bool TapeDeck::checkArrived() {
    return std::abs(head_->currentBlock() - targetBlock_) <= 1;
}

}} // namespace tapefs::firmware
