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

#include <catch2/catch_approx.hpp>

namespace visage_test {
  // Rasterized channel values compared with a margin instead of bit-exactly.
  //
  // The expected values in these tests were recorded on desktop GPUs, which
  // rasterize with 8 bits of subpixel precision. A Raspberry Pi's V3D reports
  // subPixelPrecisionBits = 6, so vertex positions snap to a grid four times
  // coarser and the coverage of a shape's edge pixels rounds a few LSBs
  // differently: a saturated channel reads 250 rather than 255, and a channel
  // that should be empty reads 6 rather than 0. The shapes are the right shapes
  // in the right places - only the last few bits move, and the gl and SDL_GPU
  // backends move them identically, which is what places the cause below both
  // APIs in the rasterizer rather than in either backend.
  //
  // 8 clears the largest deviation seen on v3d (5) with room to spare, while
  // staying far below anything that could hide a wrong colour or a missing
  // shape - those failures read as tens or hundreds of LSBs, not single digits.
  static constexpr double kChannelMargin = 8.0;

  inline Catch::Approx channel(int expected) {
    return Catch::Approx(expected).margin(kChannelMargin);
  }
}
