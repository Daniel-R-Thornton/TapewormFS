#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "tapefs/Types.hpp"

namespace tapefs {

class Crc32 {
public:
    /// Compute CRC-32 (ISO 3309 / IEEE 802.3) over a byte buffer.
    static uint32_t compute(const uint8_t* data, size_t length);

    /// Overload for ByteBuffer.
    static uint32_t compute(const ByteBuffer& buf) {
        return compute(buf.data(), buf.size());
    }
};

} // namespace tapefs
