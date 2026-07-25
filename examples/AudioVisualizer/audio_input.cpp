/* Copyright Vital Audio, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "audio_input.h"

#include "visualizer.h"

#include <algorithm>
#include <cmath>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>

namespace viz {
  namespace {
    constexpr int kChannels = 2;
    constexpr double kTwoPi = 6.283185307179586;

    float noise() {
      static uint32_t state = 0x12345678u;
      state = state * 1664525u + 1013904223u;
      return static_cast<float>(static_cast<int32_t>(state >> 8) % 2000 - 1000) / 1000.0f;
    }
  }

  AudioInput::~AudioInput() {
    shutdown();
  }

  void AudioInput::initialize(Visualizer* visualizer) {
    visualizer_ = visualizer;

    // Recorded frames are drained on the render thread rather than in an audio
    // callback, so this example needs no lock-free queue. A real product would
    // use the callback and hand samples across with one.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      SDL_AudioSpec spec {};
      spec.format = SDL_AUDIO_F32;
      spec.channels = kChannels;
      spec.freq = 48000;
      stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, nullptr, nullptr);
      if (stream_) {
        sample_rate_ = static_cast<float>(spec.freq);
        SDL_ResumeAudioStreamDevice(stream_);
        source_ = Source::Device;
      }
    }
  }

  void AudioInput::shutdown() {
    if (stream_) {
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
  }

  void AudioInput::update(double delta_seconds) {
    int frames = static_cast<int>(std::min(delta_seconds, 0.1) * sample_rate_);
    if (source_ == Source::TestSignal) {
      generateTestSignal(frames);
      return;
    }

    int available = SDL_GetAudioStreamAvailable(stream_);
    if (available <= 0)
      return;

    scratch_.resize(available / sizeof(float));
    int read = SDL_GetAudioStreamData(stream_, scratch_.data(), available);
    if (read > 0)
      visualizer_->pushSamples(scratch_.data(), read / (kChannels * sizeof(float)));
  }

  void AudioInput::generateTestSignal(int frames) {
    scratch_.resize(static_cast<size_t>(frames) * kChannels);

    double inverse_rate = 1.0 / sample_rate_;
    for (int i = 0; i < frames; ++i) {
      beat_ = std::fmod(beat_ + inverse_rate, 2.0);
      sweep_ = std::fmod(sweep_ + inverse_rate, 9.0);

      // Kick on every half beat, a slow sine sweep through the mids, and a
      // noise burst on the off-beats: enough spread to fill the spectrum.
      double kick_phase = std::fmod(beat_, 0.5);
      float kick_env = static_cast<float>(std::exp(-kick_phase * 18.0));
      float hat_env = static_cast<float>(std::exp(-std::fmod(beat_ + 0.25, 0.5) * 60.0));

      phase_ = std::fmod(phase_ + 55.0 * inverse_rate, 1.0);
      float bass = static_cast<float>(std::sin(kTwoPi * phase_)) * kick_env * 0.7f;

      double sweep_hz = 180.0 * std::pow(2.0, 4.5 * (0.5 - 0.5 * std::cos(kTwoPi * sweep_ / 9.0)));
      mid_phase_ = std::fmod(mid_phase_ + sweep_hz * inverse_rate, 1.0);
      float mid = static_cast<float>(std::sin(kTwoPi * mid_phase_)) * 0.22f;
      float hat = noise() * hat_env * 0.16f;

      scratch_[i * kChannels + 0] = bass + mid + hat;
      scratch_[i * kChannels + 1] = bass + mid * 0.6f + noise() * hat_env * 0.16f;
    }

    visualizer_->pushSamples(scratch_.data(), frames);
  }
}
