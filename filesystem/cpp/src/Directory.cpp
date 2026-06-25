#include "tapefs/Directory.hpp"
#include <cstring>
#include <algorithm>

namespace tapefs {

Result<bool> Directory::add(const std::string& filename, uint32_t fileSize,
                            BlockNumber startBlock, BlockNumber endBlock) {
    if (entries_.size() >= kMaxFiles) {
        return {{false, "directory full"}, false};
    }
    for (const auto& e : entries_) {
        if (e.filename == filename) {
            return {{false, "file exists"}, false};
        }
    }
    entries_.push_back({
        filename.substr(0, kFilenameLen - 1),
        startBlock, endBlock, fileSize
    });
    return {{true}, true};
}

const DirEntry* Directory::find(const std::string& filename) const {
    for (const auto& e : entries_) {
        if (e.filename == filename) return &e;
    }
    return nullptr;
}

Status Directory::remove(const std::string& filename) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->filename == filename) {
            entries_.erase(it);
            return {true};
        }
    }
    return {false, "not found"};
}

ByteBuffer Directory::serialise() const {
    ByteBuffer buf;
    buf.reserve(4 + entries_.size() * 32);
    buf.insert(buf.end(), {'T', 'W', 'F'});
    buf.push_back(static_cast<uint8_t>(entries_.size()));

    auto put32 = [&](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            buf.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
        }
    };

    for (const auto& e : entries_) {
        // Filename padded to 20 bytes
        for (size_t i = 0; i < kFilenameLen; i++) {
            buf.push_back(i < e.filename.size() ? e.filename[i] : '\0');
        }
        put32(e.startBlock);
        put32(e.endBlock);
        put32(e.fileSize);
    }
    return buf;
}

Directory Directory::deserialise(const ByteBuffer& data) {
    Directory dir;
    if (data.size() < 4) return dir;
    if (data[0] != 'T' || data[1] != 'W' || data[2] != 'F') return dir;

    size_t count = std::min<size_t>(data[3], kMaxFiles);
    size_t pos = 4;

    auto get32 = [&]() -> uint32_t {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            if (pos + i < data.size()) {
                v |= static_cast<uint32_t>(data[pos + i]) << (i * 8);
            }
        }
        pos += 4;
        return v;
    };

    for (size_t i = 0; i < count; i++) {
        if (pos + 32 > data.size()) break;
        DirEntry e;
        // Filename
        auto raw = reinterpret_cast<const char*>(data.data() + pos);
        e.filename.assign(raw, strnlen(raw, kFilenameLen));
        pos += kFilenameLen;
        e.startBlock = get32();
        e.endBlock   = get32();
        e.fileSize   = get32();
        dir.entries_.push_back(std::move(e));
    }
    return dir;
}

} // namespace tapefs
