#include "tapefs/firmware/DummyMcu.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <vector>
#include <poll.h>
#include <unistd.h>

namespace tapefs { namespace firmware {

// ================================================================== //
//  Packet protocol constants
// ================================================================== //

static constexpr uint8_t kStart = 0xFE;
static constexpr uint8_t kEsc   = 0xFD;

static constexpr uint8_t kCmdPing        = 0x01;
static constexpr uint8_t kCmdGetStatus   = 0x02;
static constexpr uint8_t kCmdSeek        = 0x03;
static constexpr uint8_t kCmdRewind      = 0x04;
static constexpr uint8_t kCmdWriteBlock  = 0x05;
static constexpr uint8_t kCmdReadNext    = 0x06;
static constexpr uint8_t kCmdFlush       = 0x07;
static constexpr uint8_t kCmdStop        = 0x08;

static constexpr uint8_t kRspPing        = 0x81;
static constexpr uint8_t kRspGetStatus   = 0x82;
static constexpr uint8_t kRspSeek        = 0x83;
static constexpr uint8_t kRspRewind      = 0x84;
static constexpr uint8_t kRspWriteBlock  = 0x85;
static constexpr uint8_t kRspReadNext    = 0x86;
static constexpr uint8_t kRspFlush       = 0x87;
static constexpr uint8_t kRspStop        = 0x88;

static constexpr uint8_t kErrUnknownCmd    = 0x01;
static constexpr uint8_t kErrInvalidState  = 0x02;
static constexpr uint8_t kErrChecksum      = 0x03;
static constexpr uint8_t kErrNoTape        = 0x07;
static constexpr uint8_t kErrWriteProtect  = 0x08;

// ================================================================== //
//  CRC-16-IBM
// ================================================================== //

uint16_t DummyMcu::crc16(const std::vector<uint8_t>& data) {
    uint16_t crc = 0;
    for (auto b : data) {
        crc ^= static_cast<uint16_t>(b) << 8;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x8005;
            else crc <<= 1;
            crc &= 0xFFFF;
        }
    }
    return crc;
}

// ================================================================== //
//  Byte escaping
// ================================================================== //

std::vector<uint8_t> DummyMcu::escape(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    for (auto b : data) {
        if (b == kStart) { out.push_back(kEsc); out.push_back(0x01); }
        else if (b == kEsc) { out.push_back(kEsc); out.push_back(0x02); }
        else { out.push_back(b); }
    }
    return out;
}

std::vector<uint8_t> DummyMcu::unescape(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == kEsc && i + 1 < data.size()) {
            if (data[i+1] == 0x01) out.push_back(kStart);
            else if (data[i+1] == 0x02) out.push_back(kEsc);
            i++;
        } else {
            out.push_back(data[i]);
        }
    }
    return out;
}

// ================================================================== //
//  Packet encode / decode
// ================================================================== //

std::vector<uint8_t> DummyMcu::respond(uint8_t rspId,
                                       const std::vector<uint8_t>& payload) {
    // Inner: [len(2) | cmd(1) | payload(N) | crc(2)]
    std::vector<uint8_t> inner;
    uint16_t length = 1 + payload.size() + 2;
    inner.push_back(length & 0xFF);
    inner.push_back((length >> 8) & 0xFF);
    inner.push_back(rspId);
    inner.insert(inner.end(), payload.begin(), payload.end());
    auto crc = crc16(inner);
    inner.push_back(crc & 0xFF);
    inner.push_back((crc >> 8) & 0xFF);

    // Outer: [0xFE | escaped(inner)]
    auto escaped = escape(inner);
    std::vector<uint8_t> packet = {kStart};
    packet.insert(packet.end(), escaped.begin(), escaped.end());
    return packet;
}

std::vector<uint8_t> DummyMcu::nak(uint8_t code) {
    return respond(0xFF, {code});
}

// ================================================================== //
//  Command handlers
// ================================================================== //

std::vector<uint8_t> DummyMcu::handlePing(const std::vector<uint8_t>&) {
    std::vector<uint8_t> ver = {
        'T','a','p','e','w','o','r','m','F','S',' ',
        'D','u','m','m','y','M','C','U',' ','v','0','.','2','\0'
    };
    return respond(kRspPing, ver);
}

std::vector<uint8_t> DummyMcu::handleGetStatus(const std::vector<uint8_t>&) {
    uint16_t flags = 0x101; // TAPE_PRESENT | POSITION_LOCKED
    if (deck_->isBusy()) flags |= 0x0A; // STATE_BUSY | TAPE_MOVING
    if (deck_->isAtBOT()) flags |= 0x100; // BOT
    if (noTape_) flags = 0;
    if (writeProtect_) flags |= 0x04;

    std::vector<uint8_t> resp;
    // TapePosition: block_number(4) + byte_offset(4) + tape_ms(4) + side(1) + confidence(1)
    auto bn = static_cast<uint32_t>(deck_->currentBlock());
    auto pos = static_cast<uint32_t>(deck_->positionMM());
    for (int i = 0; i < 4; i++) { resp.push_back((bn >> (i*8)) & 0xFF); }
    for (int i = 0; i < 4; i++) { resp.push_back(0); }
    for (int i = 0; i < 4; i++) { resp.push_back((pos >> (i*8)) & 0xFF); }
    resp.push_back(0); // side A
    resp.push_back(255); // confidence
    resp.push_back(flags & 0xFF);
    resp.push_back((flags >> 8) & 0xFF);
    resp.push_back(0); // buffer level
    return respond(kRspGetStatus, resp);
}

std::vector<uint8_t> DummyMcu::handleSeek(const std::vector<uint8_t>& payload) {
    if (payload.size() < 4) return nak(kErrUnknownCmd);
    uint32_t target = 0;
    for (int i = 0; i < 4; i++) target |= static_cast<uint32_t>(payload[i]) << (i*8);
    deck_->seekToBlock(static_cast<int>(target));
    // Tick until seek completes
    while (deck_->isBusy()) {
        deck_->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return respond(kRspSeek, {0x00});
}

std::vector<uint8_t> DummyMcu::handleRewind(const std::vector<uint8_t>&) {
    deck_->rewind();
    while (deck_->isBusy()) {
        deck_->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return respond(kRspRewind, {0x00});
}

std::vector<uint8_t> DummyMcu::handleWriteBlock(const std::vector<uint8_t>& payload) {
    if (noTape_) return nak(kErrNoTape);
    if (writeProtect_) return nak(kErrWriteProtect);
    if (payload.empty()) return nak(kErrUnknownCmd);
    writeBuffer_.push_back(payload);
    return respond(kRspWriteBlock, {0x00});
}

std::vector<uint8_t> DummyMcu::handleReadNext(const std::vector<uint8_t>&) {
    auto block = deck_->readNextBlock();
    if (!block) return nak(kErrNoTape);

    // Response: block_type(1) + block_data(N) + crc32(4)
    std::vector<uint8_t> resp;
    uint8_t blockType = block->empty() ? 0xFF : (*block)[0];
    resp.push_back(blockType);
    resp.insert(resp.end(), block->begin(), block->end());

    // CRC-32 of the block data
    auto crcVal = [](const std::vector<uint8_t>& d) -> uint32_t {
        uint32_t crc = 0xFFFFFFFF;
        for (auto b : d) {
            crc ^= b;
            for (int i = 0; i < 8; i++) {
                if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
                else crc >>= 1;
            }
        }
        return crc ^ 0xFFFFFFFF;
    }(*block);
    for (int i = 0; i < 4; i++) resp.push_back((crcVal >> (i*8)) & 0xFF);

    // Advance the simulated head
    deck_->tick();

    return respond(kRspReadNext, resp);
}

std::vector<uint8_t> DummyMcu::handleFlush(const std::vector<uint8_t>&) {
    for (const auto& data : writeBuffer_) {
        deck_->writeBuffer(data);
    }
    deck_->flush();
    writeBuffer_.clear();
    return respond(kRspFlush, {0x00});
}

std::vector<uint8_t> DummyMcu::handleStop(const std::vector<uint8_t>&) {
    deck_->stop();
    writeBuffer_.clear();
    return respond(kRspStop, {0x00});
}

// ================================================================== //
//  Command dispatcher
// ================================================================== //

std::vector<uint8_t> DummyMcu::processCommand(const std::vector<uint8_t>& rawPacket) {
    // Parse the framed packet
    if (rawPacket.empty() || rawPacket[0] != kStart)
        return nak(kErrChecksum);

    auto inner = unescape(std::vector<uint8_t>(rawPacket.begin() + 1, rawPacket.end()));
    if (inner.size() < 5) return nak(kErrChecksum);

    uint16_t length = static_cast<uint16_t>(inner[0])
                    | (static_cast<uint16_t>(inner[1]) << 8);
    if (inner.size() < 2 + length) return nak(kErrChecksum);

    uint8_t cmdId = inner[2];
    auto crcData = std::vector<uint8_t>(inner.begin(), inner.begin() + 2 + length - 2);
    auto storedCrc = static_cast<uint16_t>(inner[2 + length - 2])
                   | (static_cast<uint16_t>(inner[2 + length - 1]) << 8);
    if (crc16(crcData) != storedCrc) return nak(kErrChecksum);

    auto payload = std::vector<uint8_t>(inner.begin() + 3,
                                        inner.begin() + 3 + length - 3);

    // Tick the deck for realistic timing
    deck_->tick();

    switch (cmdId) {
        case kCmdPing:       return handlePing(payload);
        case kCmdGetStatus:  return handleGetStatus(payload);
        case kCmdSeek:       return handleSeek(payload);
        case kCmdRewind:     return handleRewind(payload);
        case kCmdWriteBlock: return handleWriteBlock(payload);
        case kCmdReadNext:   return handleReadNext(payload);
        case kCmdFlush:      return handleFlush(payload);
        case kCmdStop:       return handleStop(payload);
        default:             return nak(kErrUnknownCmd);
    }
}

// ================================================================== //
//  Raw callbacks (for tapefs::Filesystem)
// ================================================================== //

DummyMcu::DummyMcu() : deck_(std::make_unique<TapeDeck>()) {}

std::function<bool(const std::vector<uint8_t>&)> DummyMcu::rawWriteFn() {
    return [this](const std::vector<uint8_t>& data) -> bool {
        deck_->writeBuffer(data);
        deck_->flush();
        return true;
    };
}

std::function<std::optional<std::vector<uint8_t>>()> DummyMcu::rawReadFn() {
    return [this]() -> std::optional<std::vector<uint8_t>> {
        return deck_->readNextBlock();
    };
}

std::function<bool(uint32_t)> DummyMcu::rawSeekFn() {
    return [this](uint32_t blockNo) -> bool {
        deck_->seekToBlock(static_cast<int>(blockNo));
        while (deck_->isBusy()) {
            deck_->tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    };
}

// ================================================================== //
//  Send command (packet protocol)
// ================================================================== //

std::vector<uint8_t> DummyMcu::sendCommand(uint8_t cmdId,
                                           const std::vector<uint8_t>& payload) {
    auto packet = respond(cmdId | 0x80, payload); // request
    auto response = processCommand(packet);
    return response;
}

// ================================================================== //
//  Stdio mode
// ================================================================== //

void DummyMcu::runStdio() {
    fprintf(stderr, "[DummyMCU] Stdio mode — reading from stdin\n");

    std::vector<uint8_t> buf;
    while (true) {
        uint8_t byte;
        auto n = ::read(STDIN_FILENO, &byte, 1);
        if (n <= 0) break;

        buf.push_back(byte);

        // Try to find a complete packet
        auto it = std::find(buf.begin(), buf.end(), kStart);
        if (it == buf.end()) {
            if (buf.size() > 65536) buf.clear();
            continue;
        }
        if (it != buf.begin()) buf.erase(buf.begin(), it);

        // Need at least: marker(1) + len(2) + cmd(1) + crc(2) = 6
        if (buf.size() < 6) continue;

        // Quick length check — unescape and verify
        auto inner = unescape(std::vector<uint8_t>(buf.begin() + 1, buf.end()));
        if (inner.size() < 5) continue;

        uint16_t length = static_cast<uint16_t>(inner[0])
                        | (static_cast<uint16_t>(inner[1]) << 8);
        if (inner.size() < 2 + length) continue; // incomplete

        // Got a complete packet
        auto response = processCommand(buf);
        if (!response.empty()) {
            ::write(STDOUT_FILENO, response.data(), response.size());
        }

        buf.clear();
    }
}

}} // namespace tapefs::firmware
