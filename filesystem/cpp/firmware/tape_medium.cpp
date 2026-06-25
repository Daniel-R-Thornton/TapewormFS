#include "tape_medium.hpp"
#include <cstdio>
#include <cmath>
#include <algorithm>

namespace tapefs { namespace firmware {

TapeMedium::~TapeMedium() {
    if (file_) fclose(file_);
}

void TapeMedium::setPath(const std::string& path) {
    path_ = path;
}

void TapeMedium::writeSample(float sample) {
    if (!file_ && !path_.empty()) {
        file_ = fopen(path_.c_str(), "ab");
    }
    if (file_) {
        fwrite(&sample, sizeof(float), 1, file_);
        fflush(file_);
    }
    buffer_.push_back(sample);
    samplesWritten_++;
}

float TapeMedium::readSample() {
    if (readPos_ < buffer_.size()) {
        return buffer_[readPos_++];
    }
    return 0.0f; // silence at end of tape
}

void TapeMedium::seekToMM(double mm) {
    size_t samplePos = static_cast<size_t>(mm * kSamplesPerMM);
    readPos_ = std::min(samplePos, buffer_.size());
}

void TapeMedium::load() {
    if (loaded_) return;
    buffer_.clear();
    if (path_.empty()) return;

    FILE* f = fopen(path_.c_str(), "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    buffer_.resize(sz / sizeof(float));
    fread(buffer_.data(), sizeof(float), buffer_.size(), f);
    fclose(f);
    loaded_ = true;
    samplesWritten_ = buffer_.size();
}

void TapeMedium::format() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
    if (!path_.empty()) {
        file_ = fopen(path_.c_str(), "wb");
        if (file_) fclose(file_);
        file_ = nullptr;
    }
    buffer_.clear();
    samplesWritten_ = 0;
    readPos_ = 0;
    loaded_ = false;
}

}} // namespace tapefs::firmware
