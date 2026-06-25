#pragma once
/**
 * TapeMedium — a file on disk that acts as the raw magnetic tape.
 *
 * In real hardware this is the magnetic coating on the cassette.
 * Here it's a binary file of raw float32 audio samples.
 *
 * The modem encoder writes samples here (record).
 * The modem decoder reads samples from here (playback).
 */

#include <cstdint>
#include <string>
#include <vector>

namespace tapefs { namespace firmware {

class TapeMedium {
public:
    TapeMedium() = default;
    ~TapeMedium();

    /// Set the file path (creates/opens on first write)
    void setPath(const std::string& path);

    /// Write one sample to tape (like magnetizing the coating).
    /// This is what the modem encoder calls during record.
    void writeSample(float sample);

    /// Read next sample from tape (like reading the magnetic flux).
    /// This is what the modem decoder calls during playback.
    float readSample();

    /// Seek to a position on the tape (mm from BOT).
    /// Converts mm to sample position.
    void seekToMM(double mm);

    /// Returns the number of samples written.
    size_t sampleCount() const { return samplesWritten_; }

    /// Load all samples into memory (for fast access).
    void load();

    /// Get all samples (for testing).
    const std::vector<float>& allSamples() const { return buffer_; }

    /// Clear / format the tape.
    void format();

private:
    std::string path_;
    FILE* file_ = nullptr;
    std::vector<float> buffer_;
    size_t samplesWritten_ = 0;
    size_t readPos_ = 0;
    bool loaded_ = false;

    static constexpr double kSamplesPerMM = 3200.0 / 47.6; // ~67 samples/mm
};

}} // namespace tapefs::firmware
