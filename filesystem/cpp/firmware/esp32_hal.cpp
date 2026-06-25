#include "esp32_hal.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>

namespace tapefs { namespace hal {

// ================================================================== //
//  GPIO state
// ================================================================== //

static bool gpioState[40] = {false};
static bool gpioMode[40] = {false}; // false=input, true=output

void gpioSetPin(int pin, bool level) {
    if (pin >= 0 && pin < 40 && gpioMode[pin]) {
        gpioState[pin] = level;
    }
}

bool gpioGetPin(int pin) {
    if (pin >= 0 && pin < 40) return gpioState[pin];
    return false;
}

void gpioSetMode(int pin, bool output) {
    if (pin >= 0 && pin < 40) gpioMode[pin] = output;
}

// ================================================================== //
//  Timer
// ================================================================== //

struct TimerEntry {
    int intervalUs;
    std::function<void()> callback;
    int elapsed;
    bool active;
};
static std::vector<TimerEntry> timers;
static int nextTimerHandle = 1;

int timerStart(int intervalUs, std::function<void()> callback) {
    int handle = nextTimerHandle++;
    timers.push_back({intervalUs, std::move(callback), 0, true});
    return handle;
}

void timerStop(int handle) {
    for (auto& t : timers) {
        if (&t - &timers[0] == handle - 1) { t.active = false; break; }
    }
}

void timerTick() {
    for (auto& t : timers) {
        if (!t.active) continue;
        t.elapsed += 100; // assume 100us per tick
        while (t.elapsed >= t.intervalUs) {
            t.elapsed -= t.intervalUs;
            if (t.callback) t.callback();
        }
    }
}

// ================================================================== //
//  ADC — reads from in-memory buffer or file
// ================================================================== //

static std::vector<float> adcBuffer;
static int adcReadPos = 0;
static std::vector<float> adcFileBuffer;
static bool useAdcFile = false;

void adcInit(int, int) {}

void adcSetSource(const std::vector<float>& samples) {
    adcBuffer = samples;
    adcReadPos = 0;
    useAdcFile = false;
}

void adcSetSourceFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { fprintf(stderr, "ADC: can't open %s\n", path); return; }
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    f.seekg(0);
    adcFileBuffer.resize(sz / sizeof(float));
    f.read(reinterpret_cast<char*>(adcFileBuffer.data()), sz);
    adcReadPos = 0;
    useAdcFile = true;
    printf("ADC: loaded %zu samples from %s\n", adcFileBuffer.size(), path);
}

int adcRead(int) {
    float val = 0;
    if (useAdcFile) {
        if (adcReadPos < (int)adcFileBuffer.size())
            val = adcFileBuffer[adcReadPos++];
    } else {
        if (adcReadPos < (int)adcBuffer.size())
            val = adcBuffer[adcReadPos++];
    }
    // 12-bit: 0-4095,  center at 2048
    return (int)((val + 1.0f) * 2047.5f);
}

float adcReadFloat(int pin) {
    int raw = adcRead(pin);
    return (raw / 2047.5f) - 1.0f;
}

// ================================================================== //
//  I2C / DAC — writes to MCP4725, buffers output
// ================================================================== //

static std::vector<float> dacOutput;
static std::string dacOutputFile;

void i2cInit(const I2cConfig&) {}

void dacWrite(int value) {
    float sample = (value / 2047.5f) - 1.0f;
    dacOutput.push_back(sample);
    if (!dacOutputFile.empty()) {
        static FILE* f = nullptr;
        if (!f) f = fopen(dacOutputFile.c_str(), "wb");
        if (f) fwrite(&sample, sizeof(float), 1, f);
    }
}

void dacWriteFloat(float sample) {
    sample = std::clamp(sample, -1.0f, 1.0f);
    dacOutput.push_back(sample);
    if (!dacOutputFile.empty()) {
        static FILE* f = nullptr;
        if (!f) f = fopen(dacOutputFile.c_str(), "wb");
        if (f) fwrite(&sample, sizeof(float), 1, f);
    }
}

const std::vector<float>& dacGetOutput() { return dacOutput; }

void dacSetOutputFile(const char* path) {
    dacOutputFile = path;
}

// ================================================================== //
//  Time / delay
// ================================================================== //

void delayMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void delayUs(int us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

uint64_t micros() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

}} // namespace tapefs::hal
