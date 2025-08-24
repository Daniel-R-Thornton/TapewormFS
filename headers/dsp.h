#ifndef DSP_H
#define DSP_H

#ifndef DEBUG
#define DEBUG 0
#endif

// debug method wrapper
#if DEBUG
#include <stdio.h>
#define DEBUG_PRINT(fmt, ...) fprintf(stderr, fmt, __VA_ARGS__)
#else
#define DEBUG_PRINT(fmt, ...)
#endif

// dsp.h
// Digital Signal Processing utilities
#include <cmath>
#include <cstdint>
#include <vector>

namespace DSP
{
    // consts for setup
    constexpr float PI = 3.14159265358979323846f;
    constexpr float TWO_PI = 2.0f * PI;
    constexpr float HALF_PI = PI / 2.0f;

    // consts for audio
    constexpr float SAMPLE_RATE = 44100.0f;  // standard sample rate for audio
    constexpr float MAX_AMPLITUDE = 1.0f;    // maximum amplitude for normalized audio signals
    constexpr float MIN_AMPLITUDE = -1.0f;   // minimum amplitude for normalized audio signals
    constexpr float DEFAULT_DURATION = 1.0f; // default duration for generated tones in seconds

    /**
     * @brief Splits the spectrum into multiple bands.
     * @param numBands The number of frequency bands to create.
     * @param minFreq The minimum frequency of the spectrum.
     * @param maxFreq The maximum frequency of the spectrum.
     * @param sampleRate The sample rate of the audio.
     * @return A vector of floats representing the center frequency of each band.
     */
    std::vector<float> splitSpectrum(int numBands, float minFreq, float maxFreq, int sampleRate);

    /**
     * @brief Splits the spectrum with a specified tolerance for overlap.
     * @param overlapTolerance The desired tolerance for frequency band overlap in Hz.
     * @param minFreq The minimum frequency of the spectrum.
     * @param maxFreq The maximum frequency of the spectrum.
     * @param sampleRate The sample rate of the audio.
     * @return A vector of floats representing the center frequency of each band.
     */
    std::vector<float> splitSpectrum(float overlapTolerance, float minFreq, float maxFreq, int sampleRate);

    /**
     * @brief Encodes a vector of bytes into a single vector of frequencies.
     * @param data The input vector of bytes to encode.
     * @param frequencies The predefined set of frequencies to use for encoding.
     * @return A vector of floats where each element is a frequency present in the output signal.
     */
    std::vector<float> bytesToFFT(const std::vector<uint8_t> &data, const std::vector<float> &frequencies);

    /**
     * @brief Decodes a vector of frequencies back into a vector of bytes.
     * @param fftData The input frequency data (e.g., from an FFT result).
     * @param frequencies The predefined set of frequencies used for encoding.
     * @return A vector of uint8_t containing the decoded bytes.
     */
    std::vector<uint8_t> fftToBytes(const std::vector<float> &fftData, const std::vector<float> &frequencies);

    /**
     * @brief Generates multiple sine waves for each frequency in the input vector.
     * @param frequencies The vector of frequencies for which to generate tones.
     * @param sampleRate The sample rate for the tones.
     * @param duration The duration of the tones in seconds.
     * @return A vector of vectors, where each inner vector is a sine wave for one frequency.
     */
    std::vector<std::vector<float>> generateTones(const std::vector<float> &frequencies, int sampleRate, float duration);

    /**
     * @brief Generates a single sine wave.
     * @param frequency The frequency of the sine wave in Hz.
     * @param sampleRate The sample rate for the sine wave.
     * @param duration The duration of the sine wave in seconds.
     * @param amplitude The amplitude of the sine wave, defaulting to 1.0f.
     * @param phaseOffset The phase offset in radians, defaulting to 0.0f.
     * @return A vector of floats representing the sine wave.
     */
    std::vector<float> generateSineWave(float frequency, int sampleRate, float duration, float amplitude = MAX_AMPLITUDE, float phaseOffset = 0.0f);

    /**
     * @brief Normalizes the input vector to a specified range.
     * @param input The vector of floats to normalize.
     * @param targetMin The minimum value of the target range, defaulting to 0.0f.
     * @param targetMax The maximum value of the target range, defaulting to 1.0f.
     * @return A new vector containing the normalized data.
     */
    std::vector<float> normalize(const std::vector<float> &input, float targetMin = 0.0f, float targetMax = 1.0f);

    /**
     * @brief Combines multiple tones into a single audio signal.
     * @param tones The vector of vectors, where each inner vector is a tone.
     * @param amplitude The amplitude of the combined signal, defaulting to 1.0f.
     * @param phaseOffset The phase offset in radians, defaulting to 0.0f.
     * @return A single vector of floats representing the combined audio signal.
     */
    std::vector<float> combineTones(const std::vector<std::vector<float>> &tones, float amplitude = MAX_AMPLITUDE, float phaseOffset = 0.0f);

    /**
     * @brief Encodes a vector of bytes into an audio signal.
     * @param data The input vector of bytes to encode.
     * @param frequencies The predefined frequencies to use for encoding.
     * @param sampleRate The sample rate of the output signal.
     * @param duration The duration of the encoded signal in seconds, defaulting to 1.0f.
     * @return A vector of floats representing the encoded audio signal.
     */
    std::vector<float> encodeBytesToFrequencies(const std::vector<uint8_t> &data, const std::vector<float> &frequencies, int sampleRate, float duration = 1.0f);

    /**
     * @brief Decodes an audio signal back into a vector of bytes.
     * @param fftData The frequency spectrum data from the audio signal (e.g., from an FFT).
     * @param frequencies The predefined frequencies used for encoding.
     * @param sampleRate The sample rate of the audio signal.
     * @param duration The duration of the signal in seconds, defaulting to 1.0f.
     * @return A vector of uint8_t containing the decoded bytes.
     */
    std::vector<uint8_t> decodeFrequenciesToBytes(const std::vector<float> &fftData, const std::vector<float> &frequencies, int sampleRate, float duration = 1.0f);

    // --- Signal Analysis and Generation ---
    /**
     * @brief Performs a Fast Fourier Transform (FFT) on a time-domain signal.
     * @param timeDomainData The input audio signal as a vector of floats.
     * @param sampleRate The sample rate of the signal.
     * @return A vector of floats representing the frequency spectrum.
     */
    std::vector<float> performFFT(const std::vector<float> &timeDomainData, int sampleRate);

    /**
     * @brief Detects the presence of reference frequencies within a frequency spectrum.
     * @param fftData The frequency spectrum data (e.g., from an FFT).
     * @param referenceFrequencies The predefined frequencies to look for.
     * @param tolerance The tolerance for matching frequencies in Hz.
     * @return A vector of floats containing the detected reference frequencies.
     */
    std::vector<float> detectFrequencies(const std::vector<float> &fftData, const std::vector<float> &referenceFrequencies, float tolerance);

    /**
     * @brief Generates a period of silence.
     * @param duration The duration of the silence in seconds.
     * @param sampleRate The sample rate of the output signal.
     * @return A vector of floats representing silence (all zeros).
     */
    std::vector<float> generateSilence(float duration, int sampleRate);

    /**
     * @brief Appends one audio signal to the end of another.
     * @param mainSignal The signal to which the tone will be appended.
     * @param tone The signal to append.
     */
    void appendAudio(std::vector<float> &mainSignal, const std::vector<float> &tone);

} // namespace DSP

#endif