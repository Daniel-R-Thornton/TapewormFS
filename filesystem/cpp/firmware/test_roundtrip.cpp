#include <unistd.h>
/**
 * Round-trip test: bytes → encode → file → decode → bytes
 *
 * This proves the firmware code works EXACTLY as it would on
 * the ESP32.  The only difference is the HAL backend:
 *   - Desktop: files on disk
 *   - ESP32:   I2C DAC + onboard ADC
 *
 * The modem code (modem_encoder.cpp, modem_decoder.cpp) is
 * IDENTICAL in both cases.
 */

#include "firmware.hpp"
#include "esp32_hal.hpp"
#include "modem_encoder.hpp"
#include "modem_decoder.hpp"
#include "tape_medium.hpp"
#include "esp32_hal.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>

using namespace tapefs::firmware;
namespace hal = tapefs::hal;

static int passed = 0, failed = 0;

#define TEST(name) printf("  %-50s ... ", name); fflush(stdout)
#define PASS() do { printf("OK\n"); passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); failed++; } while(0)

void test_encode_decode_roundtrip() {
    TEST("Modem: encode → raw file → decode");

    // Create test data
    std::vector<uint8_t> testData = {'H', 'e', 'l', 'l', 'o', ' ', 'T', 'a', 'p', 'e', '!'};

    // Set HAL DAC to write to a temp file
    hal::dacSetOutputFile("/tmp/tape_test.raw");

    // Encode — same call that runs on ESP32 timer ISR
    ModemEncoder encoder;
    encoder.startEncoding(testData);
    while (encoder.isEncoding()) encoder.generateSample();

    // Get the raw audio samples
    auto& samples = hal::dacGetOutput();
    if (samples.empty()) { FAIL("no samples"); return; }
    printf(" (%zu samples, %.1fs) ", samples.size(),
           samples.size() / 3200.0);

    // Decode — same call that runs on ESP32
    ModemDecoder decoder;
    decoder.startDecoding();
    for (auto s : samples) decoder.feedSample(s);
    auto result = decoder.takeFrame();

    if (result.empty()) { FAIL("no decoded data"); return; }
    if (result != testData) {
        FAIL(("mismatch: got " + std::to_string(result.size()) +
              " bytes, expected " + std::to_string(testData.size())).c_str());
        return;
    }

    PASS();
}

void test_tape_file_persistence() {
    TEST("TapeMedium: write to file → read back");

    TapeMedium tape;
    tape.setPath("/tmp/tape_test_medium.raw");
    tape.format();

    // Write samples
    for (int i = 0; i < 1000; i++) {
        tape.writeSample(std::sin(2.0 * M_PI * 1000.0 * i / 3200.0));
    }

    // Create a new TapeMedium and load the file
    TapeMedium tape2;
    tape2.setPath("/tmp/tape_test_medium.raw");
    tape2.load();

    if (tape2.sampleCount() != 1000) {
        FAIL(("expected 1000 samples, got " +
              std::to_string(tape2.sampleCount())).c_str());
        return;
    }

    // Verify first few samples
    float s0 = tape2.allSamples()[0];
    if (std::abs(s0) < 0.001f) {
        FAIL("sample looks wrong");
        return;
    }

    PASS();
}

void test_hal_dac_adc_chain() {
    TEST("HAL: DAC → file → ADC round-trip");

    // Write a sine wave via DAC
    hal::dacSetOutputFile("/tmp/tape_test_hal.raw");
    for (int i = 0; i < 1000; i++) {
        float s = std::sin(2.0 * M_PI * 440.0 * i / 3200.0);
        hal::dacWriteFloat(s);
    }

    // Read it back via ADC
    auto& dacOut = hal::dacGetOutput();
    hal::adcSetSource(dacOut);

    float firstRead = hal::adcReadFloat(0);
    if (std::abs(firstRead) < 0.001f) { FAIL("ADC read wrong"); return; }

    // Verify a few samples match
    bool match = true;
    for (int i = 0; i < 100; i++) {
        float expected = dacOut[i];
        float actual = hal::adcReadFloat(0);
        if (std::abs(expected - actual) > 0.01f) { match = false; break; }
    }
    if (!match) { FAIL("DAC→ADC mismatch"); return; }

    PASS();
}

void test_firmware_ping() {
    TEST("Firmware: PING command");

    Firmware fw;
    uint8_t pingPacket[] = {0xFE, 0x03, 0x00, 0x01, 0x39, 0x80};  // pre-computed PING
    std::vector<uint8_t> pkt(pingPacket, pingPacket + 6);

    auto response = fw.processCommand(pkt);
    if (response.empty()) { FAIL("no response"); return; }
    if (response[0] != 0xFE) { FAIL("bad marker"); return; }

    // Parse response payload
    std::vector<uint8_t> inner;
    for (size_t i = 1; i < response.size(); i++) {
        if (response[i] == 0xFD && i + 1 < response.size()) {
            if (response[i+1] == 0x01) inner.push_back(0xFE);
            else if (response[i+1] == 0x02) inner.push_back(0xFD);
            i++;
        } else inner.push_back(response[i]);
    }

    if (inner.size() < 6) { FAIL("short response"); return; }
    uint8_t cmdId = inner[2];
    if (cmdId != 0x81) { FAIL("wrong cmd"); return; }

    uint16_t len = (uint16_t)inner[0] | ((uint16_t)inner[1] << 8);
    auto payload = std::vector<uint8_t>(inner.begin() + 3, inner.begin() + 3 + len - 3);
    auto ver = std::string(payload.begin(), payload.end());
    if (ver.find("TapewormFS") == std::string::npos) {
        FAIL(("bad version: " + ver).c_str());
        return;
    }

    PASS();
}

int main() {
    printf("ESP32 Firmware Simulation — Round-Trip Tests\n");
    printf("============================================\n\n");

    printf("These tests use the SAME modem code that runs on the ESP32.\n");
    printf("The HAL backend is the only difference:\n");
    printf("  hal::dacWriteFloat()  → file (desktop) or MCP4725 (ESP32)\n");
    printf("  hal::adcReadFloat()   → file (desktop) or onboard ADC (ESP32)\n\n");

    test_encode_decode_roundtrip();
    test_tape_file_persistence();
    test_hal_dac_adc_chain();
    test_firmware_ping();

    printf("\nResults: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
