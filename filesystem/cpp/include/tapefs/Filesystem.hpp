#pragma once

#include "tapefs/Types.hpp"
#include "tapefs/Directory.hpp"
#include "tapefs/Block.hpp"
#include <functional>

namespace tapefs {

/// High-level filesystem operations over a tape transport.
///
/// Usage:
///   auto fs = Filesystem(writeCb, readCb, seekCb);
///   fs.format();
///   fs.writeFile("hello.txt", byteData);
class Filesystem {
public:
    Filesystem(WriteCallback writeFn, ReadCallback readFn, SeekCallback seekFn);

    // No copying
    Filesystem(const Filesystem&) = delete;
    Filesystem& operator=(const Filesystem&) = delete;
    Filesystem(Filesystem&&) = default;

    /// Erase tape: empty directory + EOT marker.
    Status format();

    /// Read directory from tape (rewinds first).
    Result<Directory> readDirectory();

    /// Write directory back to tape (block 0).
    Status writeDirectory(const Directory& dir);

    /// Write a file. Splits data into blocks, writes sequentially.
    Status writeFile(const std::string& filename, const ByteBuffer& data);

    /// Read a file. Returns data, or Status::ok=false if not found.
    Result<ByteBuffer> readFile(const std::string& filename);

    /// List files (= readDirectory).
    Result<Directory> listFiles();

private:
    WriteCallback writeFn_;
    ReadCallback  readFn_;
    SeekCallback  seekFn_;
    BlockNumber   nextFreeBlock_ = 0;
};

} // namespace tapefs
