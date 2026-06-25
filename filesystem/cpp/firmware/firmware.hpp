#pragma once
/**
 * Firmware — the ESP32 main loop.
 *
 * Same structure runs on the real chip.  The HAL functions
 * (hal::dacWriteFloat, hal::adcReadFloat) swap between:
 *   - Desktop: file backend
 *   - ESP32:   I2C DAC + onboard ADC
 *
 * TapeDeck provides realistic motor physics and seek timing.
 */

#include "modem_encoder.hpp"
#include "modem_decoder.hpp"
#include "tape_medium.hpp"
#include "tape_deck.hpp"
#include <cstdint>
#include <vector>
#include <functional>

namespace tapefs { namespace firmware {

class Firmware {
public:
    Firmware();

    /// Main loop — process UART commands forever.
    void run();

    /// Process one framed command packet. Returns response.
    std::vector<uint8_t> processCommand(const std::vector<uint8_t>& packet);

    /// Record one block to tape (encoder → DAC → file).
    void recordBlock(const std::vector<uint8_t>& data);

    /// Playback one block from tape (file → ADC → decoder).
    std::vector<uint8_t> playbackBlock();

    TapeMedium& tape() { return tape_; }
    TapeDeck&   deck() { return *deck_; }

private:
    ModemEncoder encoder_;
    ModemDecoder decoder_;
    TapeMedium tape_;
    std::unique_ptr<TapeDeck> deck_;
    std::vector<std::vector<uint8_t>> writeBuffer_;
    int blockNumber_ = 0;

    // Packet protocol
    std::vector<uint8_t> handlePing();
    std::vector<uint8_t> handleWriteBlock(const std::vector<uint8_t>& p);
    std::vector<uint8_t> handleReadNext();
    std::vector<uint8_t> handleFlush();
    std::vector<uint8_t> handleSeek(const std::vector<uint8_t>& p);
    std::vector<uint8_t> handleRewind();

    std::vector<uint8_t> nak(uint8_t code);
    std::vector<uint8_t> respond(uint8_t id, const std::vector<uint8_t>& p = {});
};

}} // namespace
