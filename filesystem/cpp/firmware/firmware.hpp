#pragma once
/**
 * Firmware — the ESP32 main loop.
 *
 * This is the same structure that runs on the real chip.
 * On ESP32, the HAL functions map to ESP-IDF.
 * On desktop, they map to the file/simulation backends.
 *
 * Flow:
 *   UART receive → parse command → execute → send response
 *   USART send is synchronous (block until done)
 *   Timer ISR drives DAC output and ADC input
 */

#include "modem_encoder.hpp"
#include "modem_decoder.hpp"
#include "tape_medium.hpp"
#include <cstdint>
#include <vector>
#include <functional>
#include <thread>

namespace tapefs { namespace firmware {

class Firmware {
public:
    Firmware();

    /// Main loop — runs forever, processing UART commands.
    void run();

    /// Process one command from a framed packet.
    /// Returns response packet.
    std::vector<uint8_t> processCommand(const std::vector<uint8_t>& packet);

    /// Record mode: encode a block to tape.
    void recordBlock(const std::vector<uint8_t>& data);

    /// Playback mode: decode a block from tape.
    std::vector<uint8_t> playbackBlock();

    /// The raw tape file on disk.
    TapeMedium& tape() { return tape_; }

private:
    ModemEncoder encoder_;
    ModemDecoder decoder_;
    TapeMedium tape_;
    std::vector<std::vector<uint8_t>> writeBuffer_;
    int blockNumber_ = 0;

    // Packet protocol helpers
    std::vector<uint8_t> handlePing();
    std::vector<uint8_t> handleWriteBlock(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleReadNext();
    std::vector<uint8_t> handleFlush();
    std::vector<uint8_t> handleSeek(const std::vector<uint8_t>& payload);

    std::vector<uint8_t> nak(uint8_t code);
    std::vector<uint8_t> respond(uint8_t id, const std::vector<uint8_t>& payload = {});
};

}} // namespace tapefs::firmware
