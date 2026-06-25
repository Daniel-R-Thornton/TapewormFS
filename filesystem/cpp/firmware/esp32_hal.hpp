#pragma once
/**
 * ESP32 HAL — Hardware Abstraction Layer.
 *
 * When compiled for real ESP32, these map to ESP-IDF functions.
 * When compiled on desktop, these use file/simulation backends.
 *
 * The MODEM CODE never calls these directly — it goes through
 * the HAL, so the same modem code runs on both platforms.
 */

#include <cstdint>
#include <functional>
#include <vector>

namespace tapefs { namespace hal {

// ================================================================== //
//  GPIO
// ================================================================== //

/// Fake GPIO pins — maps to ESP32 GPIO numbers
void gpioSetPin(int pin, bool level);
bool gpioGetPin(int pin);
void gpioSetMode(int pin, bool output);

// ================================================================== //
//  Timer
// ================================================================== //

/// Start a timer that fires `callback` every `intervalUs` microseconds.
/// Returns a timer handle.
int timerStart(int intervalUs, std::function<void()> callback);
void timerStop(int handle);
void timerTick();  // advance simulation by one tick

// ================================================================== //
//  ADC — read from "tape" (file or simulated signal)
// ================================================================== //

/// Init ADC on a given pin/attenuation
void adcInit(int pin, int attenuation);

/// Read one sample (12-bit 0-4095)
int adcRead(int pin);

/// Read a float in [-1, 1]
float adcReadFloat(int pin);

/// Set the source of ADC data (file path or in-memory buffer)
void adcSetSource(const std::vector<float>& samples);
void adcSetSourceFile(const char* path);

// ================================================================== //
//  I2C — write to MCP4725 DAC
// ================================================================== //

struct I2cConfig {
    int sdaPin = 21;
    int sclPin = 22;
    int clockHz = 400000;
};

void i2cInit(const I2cConfig& cfg);

/// Write a 12-bit value to the MCP4725 DAC (I2C address 0x60)
void dacWrite(int value);

/// Write a float [-1, 1] as 12-bit DAC value
void dacWriteFloat(float sample);

/// Get the in-memory output buffer (for testing)
const std::vector<float>& dacGetOutput();

/// Set the output file path (appends samples)
void dacSetOutputFile(const char* path);

// ================================================================== //
//  Time / delay
// ================================================================== //

void delayMs(int ms);
void delayUs(int us);
uint64_t micros();

}} // namespace tapefs::hal
