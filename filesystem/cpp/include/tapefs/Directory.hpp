#pragma once

#include <string>
#include <vector>
#include "tapefs/Types.hpp"

namespace tapefs {

struct DirEntry {
    std::string  filename;
    BlockNumber  startBlock = 0;
    BlockNumber  endBlock   = 0;
    uint32_t     fileSize   = 0;
};

/// Tape directory — list of files stored near start of tape.
class Directory {
public:
    Directory() = default;

    Result<bool> add(const std::string& filename, uint32_t fileSize,
                     BlockNumber startBlock, BlockNumber endBlock);
    const DirEntry* find(const std::string& filename) const;
    Status         remove(const std::string& filename);

    const auto& entries() const { return entries_; }
    size_t count() const { return entries_.size(); }

    /// Serialise to/from byte buffer for tape storage.
    ByteBuffer serialise() const;
    static Directory deserialise(const ByteBuffer& data);

private:
    std::vector<DirEntry> entries_;
};

} // namespace tapefs
