#include "tapefs/SerialTransport.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <thread>
#include <chrono>
#include <vector>

// POSIX serial port
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>

namespace tapefs {

namespace {
constexpr uint8_t kStartMarker = 0xFE;
constexpr uint8_t kEscape      = 0xFD;

uint16_t crc16(const ByteBuffer& data) {
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

ByteBuffer escape(const ByteBuffer& data) {
    ByteBuffer out;
    for (auto b : data) {
        if (b == kStartMarker) { out.push_back(kEscape); out.push_back(0x01); }
        else if (b == kEscape) { out.push_back(kEscape); out.push_back(0x02); }
        else { out.push_back(b); }
    }
    return out;
}

ByteBuffer unescape(const ByteBuffer& data) {
    ByteBuffer out;
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i] == kEscape && i + 1 < data.size()) {
            if (data[i+1] == 0x01) out.push_back(kStartMarker);
            else if (data[i+1] == 0x02) out.push_back(kEscape);
            i++;
        } else {
            out.push_back(data[i]);
        }
    }
    return out;
}
} // namespace

// ---- Open serial port --------------------------------------------- //

static int openSerial(const std::string& port, int baud) {
    if (port == "stdio") {
        return 0;  // stdin (fd 0), stdout (fd 1)
    }

    int fd = ::open(port.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;

    struct termios tio{};
    tcgetattr(fd, &tio);

    // Raw mode
    cfmakeraw(&tio);
    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 0;

    // Baud rate
    speed_t speed = B115200;
    switch (baud) {
        case 9600:   speed = B9600;   break;
        case 19200:  speed = B19200;  break;
        case 38400:  speed = B38400;  break;
        case 57600:  speed = B57600;  break;
        case 115200: speed = B115200; break;
        case 230400: speed = B230400; break;
        case 921600: speed = B921600; break;
    }
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tcsetattr(fd, TCSANOW, &tio);

    return fd;
}

static bool writeAll(int fd, const ByteBuffer& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::write(fd, data.data() + sent, data.size() - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static std::optional<ByteBuffer> readSome(int fd, int timeoutMs) {
    struct pollfd pfd{fd, POLLIN, 0};
    int rc = poll(&pfd, 1, timeoutMs);
    if (rc <= 0) return std::nullopt;

    ByteBuffer buf(4096);
    auto n = ::read(fd, buf.data(), buf.size());
    if (n <= 0) return std::nullopt;
    buf.resize(n);
    return buf;
}

// ---- Constructor / destructor ------------------------------------- //

SerialTransport::SerialTransport(const std::string& port, int baud) {
    fd_ = openSerial(port, baud);
}

SerialTransport::~SerialTransport() { close(); }

void SerialTransport::close() {
    if (fd_ > 0) { ::close(fd_); fd_ = -1; }
}

// ---- Packet send/recv --------------------------------------------- //

bool SerialTransport::sendPacket(uint8_t cmdId, const ByteBuffer& payload) {
    if (fd_ < 0) return false;

    ByteBuffer inner;
    uint16_t length = 1 + payload.size() + 2;
    inner.push_back(static_cast<uint8_t>(length & 0xFF));
    inner.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
    inner.push_back(cmdId);
    inner.insert(inner.end(), payload.begin(), payload.end());
    auto crc = crc16(inner);
    inner.push_back(static_cast<uint8_t>(crc & 0xFF));
    inner.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    auto escaped = escape(inner);
    ByteBuffer packet;
    packet.push_back(kStartMarker);
    packet.insert(packet.end(), escaped.begin(), escaped.end());

    return writeAll(fd_, packet);
}

std::optional<ByteBuffer> SerialTransport::recvPacket(uint8_t expectedId, int timeoutMs) {
    if (fd_ < 0) return std::nullopt;

    auto deadline = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(timeoutMs);
    ByteBuffer buf;

    while (std::chrono::steady_clock::now() < deadline) {
        auto chunk = readSome(fd_, 100);
        if (chunk) {
            buf.insert(buf.end(), chunk->begin(), chunk->end());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Find marker
        auto it = std::find(buf.begin(), buf.end(), kStartMarker);
        if (it == buf.end()) {
            if (buf.size() > 65536) buf.clear();
            continue;
        }
        auto idx = static_cast<size_t>(it - buf.begin());
        if (idx > 0) buf.erase(buf.begin(), buf.begin() + idx);

        if (buf.size() < 6) continue;

        auto inner = unescape(ByteBuffer(buf.begin() + 1, buf.end()));
        if (inner.size() < 5) { buf.erase(buf.begin()); continue; }

        uint16_t length = static_cast<uint16_t>(inner[0])
                        | (static_cast<uint16_t>(inner[1]) << 8);
        if (inner.size() < 2 + length) continue;

        uint8_t cmdId = inner[2];
        uint16_t storedCrc = static_cast<uint16_t>(inner[2 + length - 2])
                           | (static_cast<uint16_t>(inner[2 + length - 1]) << 8);
        ByteBuffer crcData(inner.begin(), inner.begin() + 2 + length - 2);
        if (crc16(crcData) != storedCrc) { buf.erase(buf.begin()); continue; }

        ByteBuffer payload(inner.begin() + 3, inner.begin() + 3 + length - 3);
        if (cmdId == expectedId) return payload;
        buf.erase(buf.begin());
    }
    return std::nullopt;
}

// ---- Filesystem callbacks ----------------------------------------- //

WriteCallback SerialTransport::writeFn() {
    return [this](const ByteBuffer& data) { return writeBlock(data); };
}

ReadCallback SerialTransport::readFn() {
    return [this]() -> std::optional<ByteBuffer> { return readBlock(); };
}

SeekCallback SerialTransport::seekFn() {
    return [this](BlockNumber n) { return seek(n); };
}

bool SerialTransport::writeBlock(const ByteBuffer& data) {
    if (!sendPacket(0x05, data)) return false;
    auto rsp = recvPacket(0x85, 5000);
    if (!rsp || rsp->size() != 1 || (*rsp)[0] != 0x00) return false;
    // Flush after every write (sync write semantics)
    if (!sendPacket(0x07, {})) return false;
    rsp = recvPacket(0x87, 5000);
    return rsp && rsp->size() == 1 && (*rsp)[0] == 0x00;
}

std::optional<ByteBuffer> SerialTransport::readBlock() {
    if (!sendPacket(0x06, {})) return std::nullopt;
    auto rsp = recvPacket(0x86, 120000);  // 2 min timeout
    if (!rsp || rsp->size() <= 5) return std::nullopt;
    // Response: block_type(1) + block_data(N) + extra_crc(4)
    ByteBuffer blockData(rsp->begin() + 1, rsp->end() - 4);
    // Prepend the stored type byte to reconstruct the original block
    ByteBuffer full;
    full.push_back((*rsp)[0]);
    full.insert(full.end(), blockData.begin(), blockData.end());
    return full;
}

bool SerialTransport::seek(BlockNumber blockNo) {
    ByteBuffer payload;
    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++)
            payload.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    };
    put32(blockNo); put32(0); put32(blockNo * 1000);
    payload.push_back(0); payload.push_back(255);

    if (!sendPacket(0x03, payload)) return false;
    auto rsp = recvPacket(0x83, 30000);
    return rsp && rsp->size() == 1 && (*rsp)[0] == 0x00;
}

} // namespace tapefs
