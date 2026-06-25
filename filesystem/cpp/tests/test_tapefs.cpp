#include "tapefs/Crc32.hpp"
#include "tapefs/Ecc.hpp"
#include "tapefs/Directory.hpp"
#include "tapefs/Block.hpp"
#include "tapefs/Filesystem.hpp"
#include "tapefs/DummyMcu.hpp"
#include <cstdio>
#include <cstring>
#include <cassert>

using namespace tapefs;

// ---- Test helpers ------------------------------------------------- //

static int testsPassed = 0;
static int testsFailed = 0;

#define TEST(name) \
    do { printf("  %-45s ... ", name); } while(0)
#define PASS() \
    do { printf("OK\n"); testsPassed++; } while(0)
#define FAIL(msg) \
    do { printf("FAIL: %s\n", msg); testsFailed++; } while(0)

// ---- CRC-32 ------------------------------------------------------- //

static void testCrc32() {
    TEST("CRC32");
    auto c1 = Crc32::compute(reinterpret_cast<const uint8_t*>("Hello!"), 6);
    auto c2 = Crc32::compute(reinterpret_cast<const uint8_t*>("Hello!"), 6);
    auto c3 = Crc32::compute(reinterpret_cast<const uint8_t*>("Hello?"), 6);
    if (c1 == c2 && c1 != c3) { PASS(); }
    else { FAIL("deterministic check"); }
}

// ---- RS ECC ------------------------------------------------------- //

static void testEcc() {
    TEST("RS(255,239) encode/decode (no errors)");
    Ecc::Data data{};
    for (int i = 0; i < 239; i++) data[i] = static_cast<uint8_t>(i);

    auto parity = Ecc::encode(data);
    auto status = Ecc::decode(data, parity);
    if (status.ok) { PASS(); }
    else { FAIL(status.message.c_str()); }
}

// ---- Block serialisation ------------------------------------------ //

static void testBlockSerialise() {
    TEST("Block serialise/deserialise");

    Block block{BlockType::kData, 42, ByteBuffer{'H','e','l','l','o'}};
    auto raw = serialise(block);
    if (raw.bytes.empty()) { FAIL("serialise empty"); return; }

    auto [status, block2] = deserialise(raw);
    if (!status.ok) { FAIL(status.message.c_str()); return; }
    if (block2.type != BlockType::kData) { FAIL("type"); return; }
    if (block2.seqNo != 42) { FAIL("seqNo"); return; }
    if (block2.data.size() != 5) { FAIL("data size"); return; }

    PASS();
}

// ---- Directory ---------------------------------------------------- //

static void testDirectory() {
    TEST("Directory add/find/serialise");

    Directory dir;
    assert(dir.add("test.txt", 1024, 1, 2).status.ok);
    assert(dir.count() == 1);
    assert(dir.add("data.bin", 4096, 3, 6).status.ok);
    assert(dir.count() == 2);

    auto* e = dir.find("test.txt");
    if (!e) { FAIL("find"); return; }
    if (e->fileSize != 1024) { FAIL("fileSize"); return; }

    // Serialise round-trip
    auto buf = dir.serialise();
    auto dir2 = Directory::deserialise(buf);
    if (dir2.count() != 2) { FAIL("deserialise count"); return; }
    if (!dir2.find("data.bin")) { FAIL("deserialise find"); return; }

    PASS();
}

// ---- Filesystem integration --------------------------------------- //

static void testFilesystem() {
    TEST("Filesystem write/read via DummyMcu");

    DummyMcu mcu;
    Filesystem fs(
        [&](const ByteBuffer& b) { return mcu.rawWrite(b); },
        [&]() -> std::optional<ByteBuffer> { return mcu.rawRead(); },
        [&](BlockNumber n) { return mcu.rawSeek(n); }
    );

    auto fmt = fs.format();
    if (!fmt.ok) { FAIL(fmt.message.c_str()); return; }

    ByteBuffer content(
        reinterpret_cast<const uint8_t*>("Hello from C++ TapewormFS!"),
        reinterpret_cast<const uint8_t*>("Hello from C++ TapewormFS!") + 28
    );

    auto writeStatus = fs.writeFile("test.txt", content);
    if (!writeStatus.ok) { FAIL(writeStatus.message.c_str()); return; }

    auto [readStatus, readData] = fs.readFile("test.txt");
    if (!readStatus.ok) { FAIL(readStatus.message.c_str()); return; }

    if (readData.size() != content.size()) {
        auto msg = "size mismatch: " + std::to_string(readData.size())
                   + " vs " + std::to_string(content.size());
        FAIL(msg.c_str());
        return;
    }
    if (readData != content) {
        FAIL("data mismatch");
        return;
    }

    PASS();
}

// ---- Multiple files ----------------------------------------------- //

static void testMultipleFiles() {
    TEST("Multiple files");

    DummyMcu mcu;
    Filesystem fs(
        [&](const ByteBuffer& b) { return mcu.rawWrite(b); },
        [&]() -> std::optional<ByteBuffer> { return mcu.rawRead(); },
        [&](BlockNumber n) { return mcu.rawSeek(n); }
    );

    fs.format();
    assert(fs.writeFile("a.txt", ByteBuffer{'A','B','C'}).ok);
    assert(fs.writeFile("b.txt", ByteBuffer{'X','Y','Z'}).ok);

    auto [s, data] = fs.readFile("a.txt");
    if (!s.ok || data.size() != 3 || data[0] != 'A') {
        FAIL("a.txt"); return;
    }

    auto [s2, data2] = fs.readFile("b.txt");
    if (!s2.ok || data2.size() != 3 || data2[0] != 'X') {
        FAIL("b.txt"); return;
    }

    auto [s3, dir] = fs.listFiles();
    if (!s3.ok || dir.count() != 2) {
        FAIL("list"); return;
    }

    PASS();
}

// ---- Main --------------------------------------------------------- //

int main() {
    printf("TapewormFS C++ Tests\n");
    printf("====================\n\n");

    testCrc32();
    testEcc();
    testBlockSerialise();
    testDirectory();
    testFilesystem();
    testMultipleFiles();

    printf("\nResults: %d passed, %d failed\n", testsPassed, testsFailed);
    return testsFailed > 0 ? 1 : 0;
}
