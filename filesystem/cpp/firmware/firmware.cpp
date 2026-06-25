#include <unistd.h>
#include "firmware.hpp"
#include "esp32_hal.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cmath>

namespace tapefs { namespace firmware {

// ---- Packet protocol ---------------------------------------------- //

static constexpr uint8_t kStart = 0xFE, kEsc = 0xFD;

static uint16_t crc16(const std::vector<uint8_t>& d) {
    uint16_t c = 0;
    for (auto b : d) { c ^= b << 8; for (int i = 0; i < 8; i++) { if (c & 0x8000) c = (c << 1) ^ 0x8005; else c <<= 1; c &= 0xFFFF; } }
    return c;
}

static std::vector<uint8_t> esc(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> o;
    for (auto b : d) { if (b == kStart) { o.push_back(kEsc); o.push_back(0x01); } else if (b == kEsc) { o.push_back(kEsc); o.push_back(0x02); } else o.push_back(b); }
    return o;
}

static std::vector<uint8_t> unesc(const std::vector<uint8_t>& d) {
    std::vector<uint8_t> o;
    for (size_t i = 0; i < d.size(); i++) {
        if (d[i] == kEsc && i+1 < d.size()) { if (d[i+1] == 0x01) o.push_back(kStart); else if (d[i+1] == 0x02) o.push_back(kEsc); i++; }
        else o.push_back(d[i]);
    }
    return o;
}

// ---- Constructor -------------------------------------------------- //

Firmware::Firmware() {
    tape_.setPath("/tmp/tapewormfs_raw.tape");
    tape_.load();  // load previous tape contents
    TapeDeckConfig dcfg;
    dcfg.baudRate = 200;
    dcfg.blockSize = 1024;
    dcfg.overheadPct = 1.3;
    deck_ = std::make_unique<TapeDeck>(dcfg, &tape_);
}

// ---- Packet helpers ----------------------------------------------- //

std::vector<uint8_t> Firmware::respond(uint8_t id, const std::vector<uint8_t>& p) {
    std::vector<uint8_t> inner;
    uint16_t len = 1 + p.size() + 2;
    inner.push_back(len & 0xFF); inner.push_back((len >> 8) & 0xFF);
    inner.push_back(id);
    inner.insert(inner.end(), p.begin(), p.end());
    auto crc = crc16(inner);
    inner.push_back(crc & 0xFF); inner.push_back((crc >> 8) & 0xFF);
    auto e = esc(inner);
    std::vector<uint8_t> packet = {kStart};
    packet.insert(packet.end(), e.begin(), e.end());
    return packet;
}

std::vector<uint8_t> Firmware::nak(uint8_t code) { return respond(0xFF, {code}); }

// ---- Command handlers --------------------------------------------- //
// These use TapeDeck for realistic motor/seek timing.

std::vector<uint8_t> Firmware::handlePing() {
    return respond(0x81, {'T','a','p','e','w','o','r','m','F','S',' ','v','1','.','0','\0'});
}

std::vector<uint8_t> Firmware::handleWriteBlock(const std::vector<uint8_t>& p) {
    if (p.empty()) return nak(0x04);
    writeBuffer_.push_back(p);
    return respond(0x85, {0x00});
}

std::vector<uint8_t> Firmware::handleFlush() {
    for (const auto& data : writeBuffer_) recordBlock(data);
    writeBuffer_.clear();
    return respond(0x87, {0x00});
}

std::vector<uint8_t> Firmware::handleReadNext() {
    auto data = playbackBlock();
    if (data.empty()) return nak(0x07);

    std::vector<uint8_t> resp;
    resp.push_back(data.empty() ? 0xFF : data[0]);
    resp.insert(resp.end(), data.begin(), data.end());

    uint32_t crc = 0xFFFFFFFF;
    for (auto b : data) { crc ^= b; for (int i = 0; i < 8; i++) { if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320; else crc >>= 1; } }
    crc ^= 0xFFFFFFFF;
    for (int i = 0; i < 4; i++) resp.push_back((crc >> (i*8)) & 0xFF);

    return respond(0x86, resp);
}

std::vector<uint8_t> Firmware::handleSeek(const std::vector<uint8_t>& p) {
    if (p.size() < 4) return nak(0x04);
    uint32_t blockNo = 0;
    for (int i = 0; i < 4; i++) blockNo |= static_cast<uint32_t>(p[i]) << (i*8);

    deck_->seekToBlock(static_cast<int>(blockNo));

    // Tick until seek completes (simulated motor movement)
    while (deck_->isBusy()) {
        deck_->tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Update tape position
    tape_.seekToMM(deck_->positionMM());
    return respond(0x83, {0x00});
}

std::vector<uint8_t> Firmware::handleRewind() {
    deck_->rewind();
    while (deck_->isBusy()) { deck_->tick(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    tape_.seekToMM(0);
    return respond(0x84, {0x00});
}

// ---- Main command dispatcher ------------------------------------- //

std::vector<uint8_t> Firmware::processCommand(const std::vector<uint8_t>& packet) {
    if (packet.empty() || packet[0] != kStart) return nak(0x03);
    auto inner = unesc(std::vector<uint8_t>(packet.begin()+1, packet.end()));
    if (inner.size() < 5) return nak(0x03);
    uint16_t length = static_cast<uint16_t>(inner[0]) | (static_cast<uint16_t>(inner[1]) << 8);
    if (inner.size() < 2 + length) return nak(0x03);
    uint8_t cmdId = inner[2];
    auto cd = std::vector<uint8_t>(inner.begin(), inner.begin() + 2 + length - 2);
    auto sc = static_cast<uint16_t>(inner[2+length-2]) | (static_cast<uint16_t>(inner[2+length-1]) << 8);
    if (crc16(cd) != sc) return nak(0x03);
    auto payload = std::vector<uint8_t>(inner.begin()+3, inner.begin()+3+length-3);

    switch (cmdId) {
        case 0x01: return handlePing();
        case 0x03: return handleSeek(payload);
        case 0x04: return handleRewind();
        case 0x05: return handleWriteBlock(payload);
        case 0x06: return handleReadNext();
        case 0x07: return handleFlush();
        case 0x08: deck_->stop(); return respond(0x88, {0x00});
        default:   return nak(0x01);
    }
}

// ---- Main loop ----------------------------------------------------- //

void Firmware::run() {
    fprintf(stderr, "[Firmware] TapewormFS ESP32 simulation\n");

    std::vector<uint8_t> buf;
    while (true) {
        uint8_t byte;
        if (::read(STDIN_FILENO, &byte, 1) <= 0) break;
        buf.push_back(byte);

        auto it = std::find(buf.begin(), buf.end(), kStart);
        if (it == buf.end()) continue;
        if (it != buf.begin()) buf.erase(buf.begin(), it);
        if (buf.size() < 6) continue;

        auto inner = unesc(std::vector<uint8_t>(buf.begin()+1, buf.end()));
        if (inner.size() < 5) continue;
        uint16_t len = static_cast<uint16_t>(inner[0]) | (static_cast<uint16_t>(inner[1]) << 8);
        if (inner.size() < 2 + len) continue;

        auto response = processCommand(buf);
        if (!response.empty()) ::write(STDOUT_FILENO, response.data(), response.size());
        buf.clear();
    }
}

// ---- Record / Playback — ISR-driven --------------------------------- //
// On real ESP32, a hardware timer fires at ~3200 Hz.
// Each tick calls encoder.generateSample() or decoder.feedSample().
// In simulation, we just call it in a tight loop (same logic).

void Firmware::recordBlock(const std::vector<uint8_t>& data) {
    fprintf(stderr, "[Record] %zu bytes\n", data.size());
    hal::dacSetOutputFile("/tmp/tapewormfs_raw.tape");
    encoder_.startEncoding(data);
    while (encoder_.isEncoding()) {
        encoder_.generateSample();  // calls hal::dacWriteFloat()
    }
}

std::vector<uint8_t> Firmware::playbackBlock() {
    tape_.load();
    if (tape_.sampleCount() == 0) return {};
    hal::adcSetSource(tape_.allSamples());

    decoder_.startDecoding();
    size_t pos = 0;
    while (pos < tape_.sampleCount() && !decoder_.isFrameReady()) {
        float sample = hal::adcReadFloat(0);
        decoder_.feedSample(sample);
        pos++;
    }

    if (decoder_.isFrameReady()) {
        auto result = decoder_.takeFrame();
        fprintf(stderr, "[Playback] decoded %zu bytes\n", result.size());
        return result;
    }
    return {};
}

}} // namespace
