#include "tapefs/Filesystem.hpp"
#include "tapefs/Crc32.hpp"
#include <algorithm>

namespace tapefs {

Filesystem::Filesystem(WriteCallback writeFn, ReadCallback readFn,
                       SeekCallback seekFn)
    : writeFn_(std::move(writeFn))
    , readFn_(std::move(readFn))
    , seekFn_(std::move(seekFn)) {}

// ---- Format ------------------------------------------------------- //

Status Filesystem::format() {
    if (!seekFn_(0)) return {false, "seek failed"};

    // Empty directory
    Directory dir;
    auto raw = serialise({BlockType::kDirectory, 0, dir.serialise()});
    if (!writeFn_(raw.bytes)) return {false, "write failed"};

    // EOT marker
    raw = serialise({BlockType::kEot, 1, {}});
    if (!writeFn_(raw.bytes)) return {false, "write failed"};

    nextFreeBlock_ = 2;
    return {true};
}

// ---- Directory ---------------------------------------------------- //

Result<Directory> Filesystem::readDirectory() {
    if (!seekFn_(0)) return {{false, "seek failed"}, Directory{}};

    auto rawBytes = readFn_();
    if (!rawBytes) return {{false, "read failed"}, Directory{}};

    auto [status, block] = deserialise({*rawBytes});
    if (!status.ok) return {status, Directory{}};

    if (block.type != BlockType::kDirectory) {
        return {{false, "not a directory block"}, Directory{}};
    }

    return {{true}, Directory::deserialise(block.data)};
}

Status Filesystem::writeDirectory(const Directory& dir) {
    if (!seekFn_(0)) return {false, "seek failed"};
    auto raw = serialise({BlockType::kDirectory, 0, dir.serialise()});
    if (!writeFn_(raw.bytes)) return {false, "write failed"};
    return {true};
}

// ---- Write File --------------------------------------------------- //

Status Filesystem::writeFile(const std::string& filename,
                             const ByteBuffer& data) {
    auto [status, dir] = readDirectory();
    if (!status.ok) {
        dir = Directory{};
    }

    if (dir.find(filename)) {
        return {false, "file already exists"};
    }

    BlockNumber startBlock = nextFreeBlock_;
    if (startBlock < 1) startBlock = 1;

    if (!seekFn_(startBlock)) return {false, "seek failed"};

    // Write data blocks
    BlockNumber seq = startBlock;
    size_t offset = 0;
    while (offset < data.size()) {
        size_t chunk = std::min(data.size() - offset, kBlockDataMax);
        ByteBuffer chunkData(data.begin() + offset,
                             data.begin() + offset + chunk);

        auto raw = serialise({BlockType::kData, seq, chunkData});
        if (!writeFn_(raw.bytes)) return {false, "write failed"};

        offset += kBlockDataMax;
        seq++;
    }

    // EOT marker
    auto raw = serialise({BlockType::kEot, seq, {}});
    if (!writeFn_(raw.bytes)) return {false, "write failed"};

    // Update directory
    auto addResult = dir.add(filename, data.size(), startBlock, seq - 1);
    if (!addResult.status.ok) return addResult.status;

    auto writeDirStatus = writeDirectory(dir);
    if (!writeDirStatus.ok) return writeDirStatus;

    nextFreeBlock_ = seq + 1;
    return {true};
}

// ---- Read File ---------------------------------------------------- //

Result<ByteBuffer> Filesystem::readFile(const std::string& filename) {
    auto [status, dir] = readDirectory();
    if (!status.ok) return {status, {}};

    auto* entry = dir.find(filename);
    if (!entry) return {{false, "not found"}, {}};

    if (!seekFn_(entry->startBlock)) return {{false, "seek failed"}, {}};

    ByteBuffer result;
    for (BlockNumber seq = entry->startBlock; seq <= entry->endBlock; seq++) {
        auto rawBytes = readFn_();
        if (!rawBytes) break;

        auto [s, block] = deserialise({*rawBytes});
        if (!s.ok) continue; // skip corrupt blocks

        if (block.type == BlockType::kEot) break;
        if (block.type != BlockType::kData) continue;

        result.insert(result.end(), block.data.begin(), block.data.end());
    }

    return {{true}, std::move(result)};
}

Result<Directory> Filesystem::listFiles() {
    return readDirectory();
}

} // namespace tapefs
