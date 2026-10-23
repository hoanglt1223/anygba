#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

struct SDL_AudioStream;

namespace anyport::platform {

struct AudioConfig {
    int sample_rate = 0;  // source rate in Hz; SDL resamples to the device rate
    int channels = 2;
};

// Signed 16-bit interleaved output stream on the default playback device.
class AudioStream {
public:
    explicit AudioStream(const AudioConfig& config);
    ~AudioStream();

    AudioStream(const AudioStream&) = delete;
    AudioStream& operator=(const AudioStream&) = delete;

    // `samples` holds whole frames (a multiple of the channel count).
    void push(std::span<const std::int16_t> samples);

    // Frames pushed but not yet consumed by the device.
    std::size_t queued_frames() const;

    int sample_rate() const noexcept { return config_.sample_rate; }
    int channels() const noexcept { return config_.channels; }

private:
    SDL_AudioStream* stream_ = nullptr;
    AudioConfig config_;
};

}  // namespace anyport::platform
