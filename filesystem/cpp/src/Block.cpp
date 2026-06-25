#include "tapefs/Block.hpp"
#include "tapefs/Crc32.hpp"
#include "tapefs/Ecc.hpp"
#include <cstring>
#include <algorithm>

namespace tapefs {

static void put32(ByteBuffer& buf, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
}

static uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

RawBlock serialise(const Block& block) {
    ByteBuffer buf;
    buf.reserve(kBlockSizeMax);

    // Header
    buf.push_back(static_cast<uint8_t>(block.type));
    put32(buf, block.seqNo);

    // Payload
    size_t dataLen = std::min(block.data.size(), kBlockDataMax);
    buf.insert(buf.end(), block.data.begin(), block.data.begin() + dataLen);

    // RS parity in 239-byte chunks
    size_t total = kBlockHeader + dataLen;
    size_t offset = 0;

    while (offset < total) {
        size_t chunk = std::min(total - offset, kRsDataBytes);

        Ecc::Data rsData{};
        std::copy(buf.begin() + offset,
                  buf.begin() + offset + chunk,
                  rsData.begin());

        auto parity = Ecc::encode(rsData);
        buf.insert(buf.end(), parity.begin(), parity.end());
        offset += kRsDataBytes;
    }

    // CRC-32
    auto crc = Crc32::compute(buf);
    put32(buf, crc);

    return {std::move(buf)};
}

Result<Block> deserialise(const RawBlock& raw) {
    const auto& buf = raw.bytes;
    if (buf.size() < kBlockHeader + kCrcBytes) {
        return {{false, "too short"}, Block{}};
    }

    // Verify CRC
    auto crcData = ByteBuffer(buf.begin(), buf.end() - kCrcBytes);
    auto storedCrc = get32(buf.data() + buf.size() - kCrcBytes);
    if (Crc32::compute(crcData) != storedCrc) {
        return {{false, "CRC mismatch"}, Block{}};
    }

    // Parse header
    Block block;
    block.type  = static_cast<BlockType>(buf[0]);
    block.seqNo = get32(buf.data() + 1);

    // Find payload boundaries
    bool found = false;
    for (int chunks = 0; chunks <= static_cast<int>(kBlockSizeMax / kEccParity); chunks++) {
        size_t parityBytes = static_cast<size_t>(chunks) * kEccParity;
        size_t payloadEnd  = buf.size() - kCrcBytes - parityBytes;
        if (payloadEnd < kBlockHeader) break;

        size_t dataLen = payloadEnd - kBlockHeader;
        if (dataLen > kBlockDataMax) continue;

        size_t total = kBlockHeader + dataLen;
        size_t expected = (total + kRsDataBytes - 1) / kRsDataBytes;
        if (expected < 1 && dataLen > 0) expected = 1;

        if (static_cast<size_t>(chunks) != expected
            && !(dataLen == 0 && chunks == 0)) {
            continue;
        }

        block.data.assign(buf.begin() + kBlockHeader,
                          buf.begin() + payloadEnd);

        // RS decode each chunk
        const uint8_t* parityPtr = buf.data() + payloadEnd;
        size_t offset = 0;

        for (int c = 0; c < chunks; c++) {
            size_t chunkSize = std::min(total - offset, kRsDataBytes);

            Ecc::Data rsBuf{};
            std::copy(buf.begin() + offset,
                      buf.begin() + offset + chunkSize,
                      rsBuf.begin());

            Ecc::Parity par{};
            std::copy(parityPtr, parityPtr + kEccParity, par.begin());
            Ecc::decode(rsBuf, par);

            // Write corrected data back
            for (size_t j = 0; j < chunkSize; j++) {
                size_t global = offset + j;
                if (global >= kBlockHeader) {
                    size_t idx = global - kBlockHeader;
                    if (idx < block.data.size()) {
                        block.data[idx] = rsBuf[j];
                    }
                }
            }

            parityPtr += kEccParity;
            offset += chunkSize;
        }

        found = true;
        break;
    }

    if (!found) {
        return {{false, "invalid block layout"}, Block{}};
    }

    return {{true}, std::move(block)};
}

} // namespace tapefs
