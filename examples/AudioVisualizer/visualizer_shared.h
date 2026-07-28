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

#pragma once

#include <SDL3/SDL_timer.h>

namespace viz {
  constexpr float kPi = 3.14159265358979323846f;
  constexpr float kTwoPi = 2.0f * kPi;

  // Spectrum mapping. The GPU kernels take these as uniforms rather than
  // baking them in, so the two backends cannot drift apart.
  constexpr float kDbFloor = 80.0f;
  constexpr float kAttack = 0.5f;
  constexpr float kRelease = 0.2f;

  inline double microseconds() {
    return static_cast<double>(SDL_GetPerformanceCounter()) * 1.0e6 /
           static_cast<double>(SDL_GetPerformanceFrequency());
  }

  inline void updateAverage(double& average, bool& seeded, double sample) {
    average = seeded ? average * 0.9 + sample * 0.1 : sample;
    seeded = true;
  }
}
