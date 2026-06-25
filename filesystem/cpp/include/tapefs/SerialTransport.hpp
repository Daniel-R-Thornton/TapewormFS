#pragma once

#include "tapefs/Types.hpp"
#include <string>
#include <cstdint>

namespace tapefs {

/// Serial transport — talks framed packets over a serial port (UART).
///
/// Packet format: [0xFE | len(2B LE) | cmd_id(1B) | payload | crc16(2B)]
/// with byte escaping: 0xFE → FD 01, 0xFD → FD 02
///
/// Usage:
///   SerialTransport transport("/dev/ttyUSB0", 115200);
///   Filesystem fs(transport.writeFn(), transport.readFn(), transport.seekFn());
class SerialTransport {
public:
    /// Open a serial port. Pass "stdio" for stdin/stdout (testing).
    SerialTransport(const std::string& port, int baud = 115200);
    ~SerialTransport();

    SerialTransport(const SerialTransport&) = delete;
    SerialTransport& operator=(const SerialTransport&) = delete;

    /// Return callbacks for Filesystem.
    WriteCallback writeFn();
    ReadCallback  readFn();
    SeekCallback  seekFn();

    void close();

private:
    int fd_{-1};  // serial port file descriptor

    bool sendPacket(uint8_t cmdId, const ByteBuffer& payload);
    std::optional<ByteBuffer> recvPacket(uint8_t expectedId, int timeoutMs = 5000);

    bool writeBlock(const ByteBuffer& data);
    std::optional<ByteBuffer> readBlock();
    bool seek(BlockNumber blockNo);
};

} // namespace tapefs
