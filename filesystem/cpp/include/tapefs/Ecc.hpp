#pragma once

#include <array>
#include "tapefs/Types.hpp"

namespace tapefs {

/// RS(255,239) Reed-Solomon error correction.
/// Encodes 239 data bytes into 16 parity bytes.
class Ecc {
public:
    using Data   = std::array<uint8_t, kRsDataBytes>;
    using Parity = std::array<uint8_t, kEccParity>;

    /// Produce 16 parity bytes from 239 data bytes.
    static Parity encode(const Data& data);

    /// Attempt to correct errors in `data` using `parity`.
    /// Returns Status::ok=true if no errors (or corrected).
    static Status decode(Data& data, const Parity& parity);
};

} // namespace tapefs
