#include "tapefs/Crc32.hpp"
#include <array>

namespace tapefs {

namespace {
constexpr std::array<uint32_t, 256> buildTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc >>= 1;
            }
        }
        table[i] = crc;
    }
    return table;
}
constexpr auto kTable = buildTable();
} // namespace

uint32_t Crc32::compute(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < length; i++) {
        crc = kTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFUL;
}

} // namespace tapefs
