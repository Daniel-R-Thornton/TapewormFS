#pragma once

#include "TapeDeck.hpp"
#include <cstdint>
#include <vector>
#include <functional>
#include <optional>
#include <memory>
#include <string>

namespace tapefs { namespace firmware {

/// High-level DummyMCU that uses the full firmware simulation.
///
/// Wraps TapeDeck (Motor + TapeHead + Modem) with the packet protocol
/// and simulated hardware pins.  Connect to tapefs::Filesystem via
/// the raw callbacks, or run as a standalone process via runStdio().
///
/// Usage (unit test):
///   DummyMcu mcu;
///   Filesystem fs(mcu.rawWriteFn(), mcu.rawReadFn(), mcu.rawSeekFn());
///
/// Usage (standalone stdio):
///   DummyMcu mcu;
///   mcu.runStdio();
class DummyMcu {
public:
    DummyMcu();

    // ---- Raw block callbacks (connect to tapefs::Filesystem) ----- //

    std::function<bool(const std::vector<uint8_t>&)> rawWriteFn();
    std::function<std::optional<std::vector<uint8_t>>()> rawReadFn();
    std::function<bool(uint32_t)> rawSeekFn();

    // ---- Packet protocol callbacks ------------------------------- //

    /// Send a framed command packet, get response.
    std::vector<uint8_t> sendCommand(uint8_t cmdId,
                                     const std::vector<uint8_t>& payload = {});

    /// Run stdio mode: read framed packets from stdin, write to stdout.
    void runStdio();

    // ---- Inspection (for tests) ---------------------------------- //

    const TapeDeck& deck() const { return *deck_; }
    size_t blockCount() const { return deck_->blockCount(); }

    // ---- Error injection ----------------------------------------- //

    void setCrcErrorRate(int everyN) { crcErrorRate_ = everyN; }
    void setNoTape(bool v) { noTape_ = v; }
    void setWriteProtect(bool v) { writeProtect_ = v; }

private:
    std::unique_ptr<TapeDeck> deck_;
    int  blockNumber_ = 0;       // next block number to assign
    int  position_ = 0;
    int  opCount_ = 0;
    int  crcErrorRate_ = 0;
    bool noTape_ = false;
    bool writeProtect_ = false;

    // Write buffer (for WRITE_BLOCK + FLUSH)
    std::vector<std::vector<uint8_t>> writeBuffer_;

    // Packet protocol helpers
    static uint16_t crc16(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> escape(const std::vector<uint8_t>& data);
    static std::vector<uint8_t> unescape(const std::vector<uint8_t>& data);

    // Command handlers
    std::vector<uint8_t> handlePing(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleGetStatus(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleSeek(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleRewind(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleWriteBlock(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleReadNext(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleFlush(const std::vector<uint8_t>& payload);
    std::vector<uint8_t> handleStop(const std::vector<uint8_t>& payload);

    std::vector<uint8_t> nak(uint8_t code);
    std::vector<uint8_t> respond(uint8_t rspId,
                                 const std::vector<uint8_t>& payload = {});
    std::vector<uint8_t> processCommand(const std::vector<uint8_t>& rawPacket);
};

}} // namespace tapefs::firmware
