#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tapefs {

// ---- Constants ----------------------------------------------------- //

constexpr size_t kBlockHeader      = 5;   // type(1) + seqNo(4)
constexpr size_t kBlockDataMax     = 1000;
constexpr size_t kEccParity        = 16;
constexpr size_t kCrcBytes         = 4;
constexpr size_t kBlockSizeMax     = kBlockHeader + kBlockDataMax
                                   + kEccParity + kCrcBytes;
constexpr size_t kMaxFiles         = 32;
constexpr size_t kFilenameLen      = 20;
constexpr size_t kRsDataBytes      = 239;
constexpr int    kModemSampleRate  = 3200;
constexpr int    kModemSymbolsPerSec = 50;

// ---- Enums -------------------------------------------------------- //

enum class BlockType : uint8_t {
    kData      = 0x01,
    kDirectory = 0x02,
    kFat       = 0x03,
    kEcc       = 0x04,
    kEot       = 0xFF,
};

// ---- Result type (no error codes) --------------------------------- //

struct Status {
    bool   ok{true};
    std::string message{};
};

template<typename T>
struct Result {
    Status status;
    T      value{};
};

// ---- Type aliases ------------------------------------------------- //

using BlockNumber = uint32_t;
using ByteBuffer  = std::vector<uint8_t>;
using AudioBuffer = std::vector<float>;
using WriteCallback = std::function<bool(const ByteBuffer&)>;
using ReadCallback  = std::function<std::optional<ByteBuffer>()>;
using SeekCallback  = std::function<bool(BlockNumber)>;

// ---- Config structs ----------------------------------------------- //

struct ModemConfig {
    int               sampleRate       = kModemSampleRate;
    int               symbolsPerSecond = kModemSymbolsPerSec;
    double            pilotFreqHz      = 62.5;
    double            pilotAmplitude   = 0.15;
    int               syncSymbols      = 4;
    int               guardSamples     = 6;
    std::vector<int>  fskTonesHz       = {400, 600, 800, 1000,
                                          1150, 1300, 1450, 1550};
};

struct TapeChannelConfig {
    double wowDepthHz     = 0.5;
    double wowDepthPct    = 1.5;
    double flutterDepthHz = 4.0;
    double flutterDepthPct = 0.5;
    double noiseDb        = -25.0;
    double hum50HzDb      = -50.0;
    double dropoutRate    = 0.005;
    double saturation     = 0.85;
    double biasNoiseDb    = -55.0;
};

} // namespace tapefs
