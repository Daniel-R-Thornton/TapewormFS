#ifndef IO_H
#define IO_H

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

// IO.h
// Tape I/O utilities and encoding
#include <cstdint>
#include <vector>
#include <string>
#include <map>
#include <iostream>

/**
 * @namespace IO
 * @brief Contains structures and utilities for tape-based data storage and I/O operations.
 */
namespace IO
{
    /**
     * @brief Holds overall metadata for a tape.
     * @param sampleRate The audio sample rate of the tape.
     * @param totalDuration The total duration of the tape in seconds.
     * @param totalFrames The total number of frames on the tape.
     * @param totalBytes The total data capacity in bytes.
     * @param startingFrameId The unique ID of the first frame.
     * @param frequencies The list of frequencies used for encoding.
     * @param badFrames A list of frame IDs that are considered corrupt.
     */
    struct TapeGlobalMetadata
    {
        int sampleRate;
        float totalDuration;
        int totalFrames;
        int totalBytes;
        int capacity;
        int startingFrameId;
        std::vector<float> frequencies;
        std::vector<int> badFrames;
    };

    /**
     * @brief Metadata for a single tape frame. This is intended to be written to one channel of the tape.
     * @param id The unique identifier for the frame.
     * @param duration The duration of the frame in seconds.
     * @param frequency The frequency at which the frame is recorded.
     * @param offset The offset in the tape where this frame starts.
     * @param dataSize The size of the data in bytes.
     * @param eccBits The number of error correction bits used for this frame.
     */
    struct TapeFrameMetadata
    {
        int id;
        float duration;
        float frequency;
        int offset;
        int dataSize;
        int eccBits;
    };

    /**
     * @brief Represents a single frame of tape data, containing both data and metadata.
     */
    struct TapeFrame
    {
        std::vector<uint8_t> data;
        TapeFrameMetadata metadata;
    };

    /**
     * @brief Represents a class for controlling and interacting with a tape-like storage medium.
     *
     * This class encapsulates the state and behavior of a tape drive, including its in-memory
     * representation, current location, and global metadata.
     */
    class TapeController
    {
    public:
        // Constructor to initialize the controller with global tape metadata.
        TapeController(const TapeGlobalMetadata &metadata);

        // --- Core I/O Operations ---

        /**
         * @brief Writes a frame of data to the tape.
         * @return True if the write was successful, false otherwise.
         */
        bool writeFrame(const TapeFrame &frame);

        /**
         * @brief Reads a frame from the current location.
         * @return The TapeFrame read from the tape. Returns a default-constructed
         * TapeFrame if the read fails.
         */
        TapeFrame readFrame();

        /**
         * @brief Reads a stream of frames from the current location.
         * @param numFrames The number of frames to read.
         * @return A vector of TapeFrame objects.
         */
        std::vector<TapeFrame> readStream(int numFrames);

        /**
         * @brief Seeks to a specific frame ID on the tape.
         * @return True if the seek was successful, false otherwise.
         */
        bool seek(int frameId);

        // --- File Management and Utility ---

        /**
         * @brief Creates and initializes a new tape file on disk.
         * @param filename The name of the file to create.
         * @param metadata The global metadata for the new tape.
         * @return True if the file was successfully created, false otherwise.
         */
        static bool createTapeFile(const std::string &filename, const TapeGlobalMetadata &metadata);

        /**
         * @brief Writes all in-memory tape frames to the physical file.
         * @return True if the flush was successful, false otherwise.
         */
        bool flush();

        /**
         * @brief Retrieves a frame by its ID from the in-memory tape data.
         * @param frameId The ID of the frame to retrieve.
         * @return The TapeFrame if found, or a default-constructed TapeFrame otherwise.
         */
        TapeFrame getFrame(int frameId);

        /**
         * @brief Seeks to a specific byte offset on the tape.
         * @param offset The byte offset to seek to.
         * @return True if the seek was successful, false otherwise.
         */
        bool seekByOffset(int offset);

        // --- Hardware-level Interactions (e.g., read/write to physical storage) ---
        bool writeToFile(const std::string &filename);
        bool readFromFile(const std::string &filename);

        // --- Getters ---
        int getCurrentLocation() const { return m_currentLocation; }
        const TapeGlobalMetadata &getMetadata() const { return m_metadata; }

    private:
        TapeGlobalMetadata m_metadata;
        std::vector<TapeFrame> m_tape; // The in-memory representation of the tape's data.
        int m_currentLocation;         // Tracks the current frame offset.

        // These can be used for encoding/decoding and serialization.
        // The public methods will call these as needed.
        std::pair<std::string, std::vector<float>> encode(const TapeFrame &frame);
        TapeFrame decode(const std::string &leftChannel, const std::vector<float> &rightChannelFrequencies);
    };

    // --- Utility Functions (still useful as standalone functions) ---
    /**
     * @brief Serializes TapeFrameMetadata into a vector of bytes for storage.
     */
    std::vector<uint8_t> serializeMetadata(const TapeFrameMetadata &metadata);

    /**
     * @brief Deserializes a vector of bytes into TapeFrameMetadata.
     */
    TapeFrameMetadata deserializeMetadata(const std::vector<uint8_t> &data);
} // namespace IO

#endif