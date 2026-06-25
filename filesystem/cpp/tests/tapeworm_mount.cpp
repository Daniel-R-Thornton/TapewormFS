/**
 * tapeworm-mount — FUSE filesystem backed by audio cassette.
 *
 * Mounts a folder on your computer that reads/writes files to a
 * cassette tape via an ESP32 over UART.
 *
 * Build:  g++ -std=c++17 -lfuse3 tapeworm-mount.cpp -o tapeworm-mount
 * Usage:  ./tapeworm-mount /mnt/tape0 /dev/ttyUSB0
 *
 * Files you copy to the mount point are cached to /tmp, then synced
 * to tape in the background.
 */

#include "tapefs/Types.hpp"
#include "tapefs/Filesystem.hpp"
#include "tapefs/SerialTransport.hpp"

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <filesystem>

using namespace tapefs;

// ---- Cache -------------------------------------------------------- //

struct CacheEntry {
    ByteBuffer data;
    bool dirty = false;
};

static std::unordered_map<std::string, CacheEntry> sCache;
static std::string sCacheDir;
static Filesystem* sFs = nullptr;
static bool sRunning = true;

static std::string cachePath(const std::string& name) {
    return sCacheDir + "/" + name;
}

static ByteBuffer cacheRead(const std::string& name) {
    auto it = sCache.find(name);
    if (it != sCache.end()) return it->second.data;

    // Try disk cache
    auto path = cachePath(name);
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        auto sz = ftell(f);
        rewind(f);
        ByteBuffer buf(sz);
        fread(buf.data(), 1, sz, f);
        fclose(f);
        sCache[name] = {buf, false};
        return buf;
    }

    // Read from tape
    auto result = sFs->readFile(name);
    if (result.status.ok) {
        sCache[name] = {result.value, false};
        // Write to disk cache
        if (!sCacheDir.empty()) {
            auto p = cachePath(name);
            FILE* f = fopen(p.c_str(), "wb");
            if (f) {
                fwrite(result.value.data(), 1, result.value.size(), f);
                fclose(f);
            }
        }
        return result.value;
    }
    return {};
}

static void cacheWrite(const std::string& name, const ByteBuffer& data) {
    sCache[name] = {data, true};
    // Write to disk cache
    if (!sCacheDir.empty()) {
        auto p = cachePath(name);
        FILE* f = fopen(p.c_str(), "wb");
        if (f) {
            fwrite(data.data(), 1, data.size(), f);
            fclose(f);
        }
    }
}

static void syncAll() {
    for (auto& [name, entry] : sCache) {
        if (entry.dirty) {
            printf("  [sync] Writing '%s' (%zu B) to tape\n",
                   name.c_str(), entry.data.size());
            auto status = sFs->writeFile(name, entry.data);
            if (status.ok) {
                entry.dirty = false;
                printf("  [sync] Done\n");
            } else {
                printf("  [sync] Failed: %s\n", status.message.c_str());
            }
        }
    }
}

static void syncLoop() {
    while (sRunning) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        syncAll();
    }
    syncAll(); // final flush
}

// ---- FUSE operations ---------------------------------------------- //

static int tape_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
    memset(st, 0, sizeof(*st));
    st->st_nlink = 2;

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        return 0;
    }

    std::string name(path + 1); // strip leading /
    auto data = cacheRead(name);
    if (data.empty()) return -ENOENT;

    st->st_mode  = S_IFREG | 0644;
    st->st_nlink = 1;
    st->st_size  = data.size();
    return 0;
}

static int tape_readdir(const char*, void* buf, fuse_fill_dir_t filler,
                        off_t, struct fuse_file_info*, enum fuse_readdir_flags) {
    filler(buf, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
    filler(buf, "..", NULL, 0, FUSE_FILL_DIR_PLUS);

    auto [status, dir] = sFs->listFiles();
    if (status.ok) {
        for (const auto& e : dir.entries()) {
            filler(buf, e.filename.c_str(), NULL, 0, FUSE_FILL_DIR_PLUS);
        }
    }
    return 0;
}

static int tape_open(const char*, struct fuse_file_info*) { return 0; }

static int tape_read(const char* path, char* buf, size_t size, off_t offset,
                     struct fuse_file_info*) {
    std::string name(path + 1);
    auto data = cacheRead(name);
    if (data.empty()) return -ENOENT;
    if (static_cast<size_t>(offset) >= data.size()) return 0;
    size_t toCopy = std::min(size, data.size() - static_cast<size_t>(offset));
    memcpy(buf, data.data() + offset, toCopy);
    return toCopy;
}

static int tape_write(const char* path, const char* buf, size_t size,
                      off_t offset, struct fuse_file_info*) {
    std::string name(path + 1);
    auto existing = cacheRead(name);
    if (existing.empty() && offset > 0) return -ENOENT;

    if (static_cast<size_t>(offset) + size > existing.size()) {
        existing.resize(offset + size);
    }
    memcpy(existing.data() + offset, buf, size);
    cacheWrite(name, existing);
    return size;
}

static int tape_truncate(const char* path, off_t length, struct fuse_file_info*) {
    std::string name(path + 1);
    auto existing = cacheRead(name);
    existing.resize(length);
    cacheWrite(name, existing);
    return 0;
}

static int tape_unlink(const char* path) {
    std::string name(path + 1);
    // Read directory, remove entry, write back
    auto [status, dir] = sFs->listFiles();
    if (!status.ok) return -EIO;
    dir.remove(name);
    sFs->writeDirectory(dir);
    sCache.erase(name);
    return 0;
}

static const struct fuse_operations tape_ops = {
    .getattr = tape_getattr,
    .readdir = tape_readdir,
    .open    = tape_open,
    .read    = tape_read,
    .write   = tape_write,
    .truncate = tape_truncate,
    .unlink  = tape_unlink,
};

// ---- Main --------------------------------------------------------- //

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <mountpoint> <serial_port> [baud]\n", argv[0]);
        fprintf(stderr, "  mountpoint:  where to mount (e.g. /mnt/tape0)\n");
        fprintf(stderr, "  serial_port: /dev/ttyUSB0 or 'stdio'\n");
        fprintf(stderr, "  baud:        115200 (default)\n\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s /mnt/tape0 /dev/ttyUSB0\n", argv[0]);
        fprintf(stderr, "  %s ./tape_mount stdio  (test with DummyMCU)\n", argv[0]);
        return 1;
    }

    std::string mountpoint = argv[1];
    std::string port       = argv[2];
    int baud = (argc > 3) ? std::stoi(argv[3]) : 115200;

    // Setup cache dir
    auto now = std::chrono::system_clock::now();
    auto t  = std::chrono::system_clock::to_time_t(now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", localtime(&t));
    sCacheDir = "/tmp/tapewormfs/" + std::string(ts);
    std::filesystem::create_directories(sCacheDir + "/files");

    // Current symlink
    std::filesystem::remove("/tmp/tapewormfs/current");
    std::filesystem::create_symlink(sCacheDir, "/tmp/tapewormfs/current");

    // Connect to ESP32
    printf("Connecting to %s at %d baud...\n", port.c_str(), baud);
    auto transport = new SerialTransport(port, baud);
    sFs = new Filesystem(transport->writeFn(), transport->readFn(),
                         transport->seekFn());

    // Start sync thread
    std::thread syncThread(syncLoop);

    // Mount FUSE
    printf("Mounting at %s...\n", mountpoint.c_str());
    printf("  Cache: %s\n", sCacheDir.c_str());
    printf("  Press Ctrl+C to unmount\n\n");

    struct fuse_args args = FUSE_ARGS_INIT(0, nullptr);
    // Build fuse args manually
    char* mountArg = strdup(mountpoint.c_str());
    char* fgArg    = strdup("-f");  // foreground

    std::vector<char*> fuseArgv = {argv[0], fgArg, mountArg};
    args.argc = static_cast<int>(fuseArgv.size());
    args.argv = fuseArgv.data();

    int ret = fuse_main(args.argc, args.argv, &tape_ops, nullptr);

    // Cleanup
    sRunning = false;
    syncThread.join();

    delete sFs;
    delete transport;
    free(mountArg);
    free(fgArg);

    return ret;
}
