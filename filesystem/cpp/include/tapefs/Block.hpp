#pragma once

#include "tapefs/Types.hpp"

namespace tapefs {

/// Logical block (before RS + CRC).
struct Block {
    BlockType  type   = BlockType::kData;
    BlockNumber seqNo = 0;
    ByteBuffer data;
};

/// Serialised block (RS parity + CRC appended).
struct RawBlock {
    ByteBuffer bytes;
};

/// Add RS parity and CRC, producing a RawBlock ready for transport.
RawBlock serialise(const Block& block);

/// Strip RS parity and CRC. Returns Block and Status.
Result<Block> deserialise(const RawBlock& raw);

} // namespace tapefs
