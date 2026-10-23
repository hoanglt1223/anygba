#include <anyport/platform/audio.h>

#include "sdl_error.h"

#include <SDL3/SDL_audio.h>

#include <string>

namespace anyport::platform {

AudioStream::AudioStream(const AudioConfig& config) : config_(config) {
    if (config.sample_rate <= 0) {
        throw Error("audio sample rate must be positive");
    }
    if (config.channels < 1 || config.channels > 8) {
        throw Error("audio channel count must be 1-8, got " + std::to_string(config.channels));
    }
    const SDL_AudioSpec spec{SDL_AUDIO_S16, config.channels, config.sample_rate};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        detail::throw_sdl_error("SDL_OpenAudioDeviceStream");
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        SDL_DestroyAudioStream(stream_);
        detail::throw_sdl_error("SDL_ResumeAudioStreamDevice");
    }
}

AudioStream::~AudioStream() {
    SDL_DestroyAudioStream(stream_);
}

void AudioStream::push(std::span<const std::int16_t> samples) {
    if (samples.size() % static_cast<std::size_t>(config_.channels) != 0) {
        throw Error("audio push of " + std::to_string(samples.size()) +
                    " samples is not a whole number of frames");
    }
    if (samples.empty()) {
        return;
    }
    if (!SDL_PutAudioStreamData(stream_, samples.data(),
                                static_cast<int>(samples.size_bytes()))) {
        detail::throw_sdl_error("SDL_PutAudioStreamData");
    }
}

std::size_t AudioStream::queued_frames() const {
    const int bytes = SDL_GetAudioStreamQueued(stream_);
    if (bytes < 0) {
        detail::throw_sdl_error("SDL_GetAudioStreamQueued");
    }
    return static_cast<std::size_t>(bytes) /
           (sizeof(std::int16_t) * static_cast<std::size_t>(config_.channels));
}

}  // namespace anyport::platform
