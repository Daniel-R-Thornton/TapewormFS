#include <unistd.h>
#include "firmware.hpp"
#include "esp32_hal.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>

namespace tapefs { namespace firmware {

// Packet protocol constants
static constexpr uint8_t kStart = 0xFE;
static constexpr uint8_t kEsc   = 0xFD;

static constexpr uint8_t kCmdPing       = 0x01;
static constexpr uint8_t kCmdWriteBlock = 0x05;
static constexpr uint8_t kCmdReadNext   = 0x06;
static constexpr uint8_t kCmdFlush      = 0x07;
static constexpr uint8_t kCmdSeek       = 0x03;

static constexpr uint8_t kRspPing       = 0x81;
static constexpr uint8_t kRspWriteBlock = 0x85;
static constexpr uint8_t kRspReadNext   = 0x86;
static constexpr uint8_t kRspFlush      = 0x87;
static constexpr uint8_t kRspSeek       = 0x83;

// ---- CRC-16 ------------------------------------------------------- //

static uint16_t crc16(const std::vector<uint8_t>& data) {
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

static std::vector<uint8_t> escape(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out;
    for (auto b : data) {
        if (b == kStart) { out.push_back(kEsc); out.push_back(0x01); }
        else if (b == kEsc) { out.push_back(kEsc); out.push_back(0x02); }
        else out.push_back(b);
    }
    return out;
}

// ---- Constructor -------------------------------------------------- //

Firmware::Firmware() {
    // Setup encoder callback — on real ESP32 this would be the timer ISR
    // that feeds samples to the MCP4725 DAC over I2C
    tape_.setPath("/tmp/tapewormfs_raw.tape");

    // Decoder callback — called when a complete frame is decoded
    decoder_.setDataCallback([this](const std::vector<uint8_t>& data) {
        // On real ESP32: send this data back over UART as READ_NEXT response
        printf("[DECODER] Frame decoded: %zu bytes\n", data.size());
    });
}

// ---- Main loop ---------------------------------------------------- //
// This is what runs on the ESP32: read UART, process, respond.

void Firmware::run() {
    fprintf(stderr, "[Firmware] TapewormFS ESP32 firmware starting\n");
    fprintf(stderr, "[Firmware] Tape file: %s\n", tape_.allSamples().empty() ?
            "not loaded" : "loaded");

    std::vector<uint8_t> buf;
    while (true) {
        // Read one byte (simulated — in real ESP32 this is UART ISR)
        // For simulation, check stdin
        uint8_t byte;
        if (::read(STDIN_FILENO, &byte, 1) <= 0) break;
        buf.push_back(byte);

        // Try to find a complete packet
        auto it = std::find(buf.begin(), buf.end(), kStart);
        if (it == buf.end()) continue;
        if (it != buf.begin()) buf.erase(buf.begin(), it);
        if (buf.size() < 6) continue;

        // Quick length check
        auto inner = [&]() -> std::vector<uint8_t> {
            std::vector<uint8_t> out;
            for (size_t i = 1; i < buf.size(); i++) {
                if (buf[i] == kEsc && i + 1 < buf.size()) {
                    if (buf[i+1] == 0x01) out.push_back(kStart);
                    else if (buf[i+1] == 0x02) out.push_back(kEsc);
                    i++;
                } else out.push_back(buf[i]);
            }
            return out;
        }();

        if (inner.size() < 5) continue;
        uint16_t len = static_cast<uint16_t>(inner[0])
                     | (static_cast<uint16_t>(inner[1]) << 8);
        if (inner.size() < 2 + len) continue;

        // Got a complete packet — process it
        auto response = processCommand(buf);
        if (!response.empty()) {
            ::write(STDOUT_FILENO, response.data(), response.size());
        }
        buf.clear();
    }
}

// ---- Command processing ------------------------------------------- //
// This is the EXACT same code that runs on the ESP32.

std::vector<uint8_t> Firmware::respond(uint8_t id,
                                       const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> inner;
    uint16_t length = 1 + payload.size() + 2;
    inner.push_back(length & 0xFF);
    inner.push_back((length >> 8) & 0xFF);
    inner.push_back(id);
    inner.insert(inner.end(), payload.begin(), payload.end());
    auto crc = crc16(inner);
    inner.push_back(crc & 0xFF);
    inner.push_back((crc >> 8) & 0xFF);

    auto escaped = escape(inner);
    std::vector<uint8_t> packet = {kStart};
    packet.insert(packet.end(), escaped.begin(), escaped.end());
    return packet;
}

std::vector<uint8_t> Firmware::nak(uint8_t code) {
    return respond(0xFF, {code});
}

std::vector<uint8_t> Firmware::handlePing() {
    std::vector<uint8_t> ver = {'T','a','p','e','w','o','r','m','F','S',' ','v','1','.','0','\0'};
    return respond(kRspPing, ver);
}

std::vector<uint8_t> Firmware::handleWriteBlock(const std::vector<uint8_t>& payload) {
    if (payload.empty()) return nak(0x04);
    writeBuffer_.push_back(payload);
    return respond(kRspWriteBlock, {0x00});
}

std::vector<uint8_t> Firmware::handleFlush() {
    for (const auto& data : writeBuffer_) {
        recordBlock(data);
    }
    writeBuffer_.clear();
    return respond(kRspFlush, {0x00});
}

std::vector<uint8_t> Firmware::handleReadNext() {
    auto data = playbackBlock();
    if (data.empty()) return nak(0x07); // end of tape

    // Build response: block_type(1) + data(N) + crc32(4)
    std::vector<uint8_t> resp;
    uint8_t blockType = data.empty() ? 0xFF : data[0];
    resp.push_back(blockType);
    resp.insert(resp.end(), data.begin(), data.end());

    // CRC-32
    uint32_t crc = 0xFFFFFFFF;
    for (auto b : data) {
        crc ^= b;
        for (int i = 0; i < 8; i++) {
            if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
            else crc >>= 1;
        }
    }
    crc ^= 0xFFFFFFFF;
    for (int i = 0; i < 4; i++) resp.push_back((crc >> (i*8)) & 0xFF);

    return respond(kRspReadNext, resp);
}

std::vector<uint8_t> Firmware::handleSeek(const std::vector<uint8_t>& payload) {
    if (payload.size() < 4) return nak(0x04);
    uint32_t blockNo = 0;
    for (int i = 0; i < 4; i++) blockNo |= static_cast<uint32_t>(payload[i]) << (i*8);

    // Seek to block: each block is ~12mm of tape
    double mmPerBlock = 12.0;
    tape_.seekToMM(blockNo * mmPerBlock);
    return respond(kRspSeek, {0x00});
}

std::vector<uint8_t> Firmware::processCommand(const std::vector<uint8_t>& packet) {
    // Parse
    if (packet.empty() || packet[0] != kStart) return nak(0x03);

    std::vector<uint8_t> inner;
    for (size_t i = 1; i < packet.size(); i++) {
        if (packet[i] == kEsc && i + 1 < packet.size()) {
            if (packet[i+1] == 0x01) inner.push_back(kStart);
            else if (packet[i+1] == 0x02) inner.push_back(kEsc);
            i++;
        } else inner.push_back(packet[i]);
    }
    if (inner.size() < 5) return nak(0x03);

    uint16_t length = static_cast<uint16_t>(inner[0])
                    | (static_cast<uint16_t>(inner[1]) << 8);
    if (inner.size() < 2 + length) return nak(0x03);

    uint8_t cmdId = inner[2];
    auto crcData = std::vector<uint8_t>(inner.begin(), inner.begin() + 2 + length - 2);
    auto storedCrc = static_cast<uint16_t>(inner[2 + length - 2])
                   | (static_cast<uint16_t>(inner[2 + length - 1]) << 8);
    if (crc16(crcData) != storedCrc) return nak(0x03);

    auto payload = std::vector<uint8_t>(inner.begin() + 3, inner.begin() + 3 + length - 3);

    switch (cmdId) {
        case kCmdPing:       return handlePing();
        case kCmdWriteBlock: return handleWriteBlock(payload);
        case kCmdReadNext:   return handleReadNext();
        case kCmdFlush:      return handleFlush();
        case kCmdSeek:       return handleSeek(payload);
        default:             return nak(0x01);
    }
}

// ---- Record / Playback -------------------------------------------- //
// These call the HAL directly — same code on ESP32.

void Firmware::recordBlock(const std::vector<uint8_t>& data) {
    fprintf(stderr, "[Record] Encoding %zu bytes to tape\n", data.size());

    // Set HAL DAC output to a file on disk
    hal::dacSetOutputFile("/tmp/tapewormfs_raw.tape");

    // This is EXACTLY how it would work on the ESP32:
    // The encoder calls hal::dacWriteFloat() for each sample.
    // On ESP32, that writes to MCP4725 over I2C.
    // On desktop, that writes to a file.
    encoder_.encode(data);

    fprintf(stderr, "[Record] Done — %zu samples written\n",
            hal::dacGetOutput().size());
}

std::vector<uint8_t> Firmware::playbackBlock() {
    // Load tape file into ADC buffer
    tape_.load();
    if (tape_.sampleCount() == 0) return {};

    // Set the ADC source to the tape buffer
    hal::adcSetSource(tape_.allSamples());

    // Decode — this calls hal::adcReadFloat() for each sample.
    // On ESP32, that reads from the real ADC.
    // On desktop, that reads from the file buffer.
    std::vector<float> adcInput;
    for (size_t i = 0; i < tape_.sampleCount(); i++) {
        adcInput.push_back(hal::adcReadFloat(0));
    }

    auto result = decoder_.decode(adcInput);
    fprintf(stderr, "[Playback] Decoded %zu bytes\n", result.size());
    return result;
}

}} // namespace tapefs::firmware
