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

#include "defines.h"

#include "string_utils.h"

#include <cstdio>

#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#include <windows.h>

namespace visage {
  // Mirrored to stderr because OutputDebugString is invisible without an
  // attached debugger, which hides real failures in normal runs.
  void debugLogString(const char* file, unsigned int line, const String& log_message) {
    std::string location = std::string(file) + " (" + std::to_string(line) + ") ";
    OutputDebugStringA(location.c_str());
    OutputDebugStringW(log_message.toWide().c_str());
    if (log_message.isEmpty() || log_message[log_message.size() - 1] != '\n')
      OutputDebugStringW(L"\n");

    std::fprintf(stderr, "%s%s\n", location.c_str(), log_message.toUtf8().c_str());
    std::fflush(stderr);
  }

  void debugLogArgs(const char* file, unsigned int line, const char* format, va_list arg_list) {
    static constexpr int kSize = 500;
    char buffer[kSize];
    std::vsnprintf(buffer, sizeof(buffer), format, arg_list);
    debugLogString(file, line, buffer);
  }

  void debugAssert(bool condition, const char* file, unsigned int line) {
    if (condition)
      return;

    debugLogString(file, line, "Assertion failed");
    __debugbreak();
  }

  void forceCrash() {
    __debugbreak();
  }
}
