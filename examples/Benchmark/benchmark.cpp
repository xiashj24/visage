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

// Compares the gl and gpu backends on the same machine. It renders headless,
// so it needs no display and measures the renderer rather than the compositor
// or the display's refresh rate.
//
// SDL_GPU exposes no timestamp queries, so GPU time cannot be read back
// directly and nothing here claims to report it. A batch of frames is
// submitted and then drained by a single readback whose fence wait guarantees
// the GPU finished all of them, which puts GPU execution inside wall time:
//
//   wall     per-frame wall clock, GPU work included
//   cpu      CPU burned across every thread, so a driver with worker threads
//            can exceed wall - that is the number to watch on a 4-core Pi
//   blocked  wall minus this thread's CPU: how long the render loop waited.
//            A lower bound on GPU-bound time, not a measurement of it.

#include "embedded/example_fonts.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <visage/app.h>
#include <visage/graphics.h>
#include <visage/windowing.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <ctime>
#endif

namespace {
#if defined(_WIN32)
  double fileTimeSeconds(const FILETIME& time) {
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart * 1e-7;  // 100ns ticks.
  }
#endif

  // Every thread the process owns. GL drivers run their own, so this can
  // exceed wall time - which is the point: it is the whole CPU cost.
  double processCpuSeconds() {
#if defined(_WIN32)
    FILETIME creation, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
      return 0.0;
    return fileTimeSeconds(kernel) + fileTimeSeconds(user);
#else
    timespec time {};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0)
      return 0.0;
    return time.tv_sec + time.tv_nsec * 1e-9;
#endif
  }

  // This thread alone, so wall minus this is the time the render loop spent
  // blocked rather than working.
  double threadCpuSeconds() {
#if defined(_WIN32)
    FILETIME creation, exit, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user))
      return 0.0;
    return fileTimeSeconds(kernel) + fileTimeSeconds(user);
#else
    timespec time {};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0)
      return 0.0;
    return time.tv_sec + time.tv_nsec * 1e-9;
#endif
  }

  double wallSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
  }

  // Every scene varies with the frame index so no two frames are identical
  // and nothing can be cached away, while staying deterministic.
  using SceneDraw = void (*)(visage::Canvas&, int width, int height, int frame);

  void drawShapes(visage::Canvas& canvas, int width, int height, int frame) {
    static constexpr int kCount = 400;
    float phase = frame * 0.01f;

    canvas.setColor(0xff101014);
    canvas.fill(0, 0, width, height);

    for (int i = 0; i < kCount; ++i) {
      float t = i / static_cast<float>(kCount);
      float x = width * (0.5f + 0.45f * std::sin(t * 12.0f + phase));
      float y = height * (0.5f + 0.45f * std::cos(t * 9.0f + phase * 1.3f));
      float size = 12.0f + 28.0f * t;

      canvas.setColor(0xff000000 | (static_cast<unsigned>(t * 255) << 16) | 0x4488);
      if (i & 1)
        canvas.roundedRectangle(x - size * 0.5f, y - size * 0.5f, size, size, size * 0.25f);
      else
        canvas.circle(x - size * 0.5f, y - size * 0.5f, size);
    }
  }

  void drawGradients(visage::Canvas& canvas, int width, int height, int frame) {
    static constexpr int kCount = 200;
    float phase = frame * 0.01f;

    canvas.setColor(visage::Brush::vertical(0xff202040, 0xff402020));
    canvas.fill(0, 0, width, height);

    for (int i = 0; i < kCount; ++i) {
      float t = i / static_cast<float>(kCount);
      float x = width * (0.5f + 0.4f * std::sin(t * 7.0f + phase));
      float y = height * (0.5f + 0.4f * std::cos(t * 5.0f + phase));
      float size = 30.0f + 50.0f * t;

      canvas.setColor(visage::Brush::horizontal(0xffff8844, 0xff4488ff));
      canvas.roundedRectangle(x, y, size, size, 8.0f);
    }
  }

  void drawText(visage::Canvas& canvas, int width, int height, int frame) {
    static constexpr int kRows = 30;
    visage::Font font(16, resources::fonts::Lato_Regular_ttf);

    canvas.setColor(0xff0a0a0a);
    canvas.fill(0, 0, width, height);

    canvas.setColor(0xffdddddd);
    for (int row = 0; row < kRows; ++row) {
      // The counter keeps glyph runs changing without thrashing the atlas.
      std::string line = "visage backend benchmark row " + std::to_string(row) + " frame " +
                         std::to_string(frame % 1000);
      float y = row * (height / static_cast<float>(kRows));
      canvas.text(line, font, visage::Font::Justification::kLeft, 8, y, width - 16,
                  height / static_cast<float>(kRows));
    }
  }

  void drawPaths(visage::Canvas& canvas, int width, int height, int frame) {
    static constexpr int kPaths = 24;
    static constexpr int kSegments = 48;
    float phase = frame * 0.02f;

    canvas.setColor(0xff05050a);
    canvas.fill(0, 0, width, height);

    for (int p = 0; p < kPaths; ++p) {
      visage::Path path;
      float offset = p * 0.2f + phase;
      for (int s = 0; s <= kSegments; ++s) {
        float t = s / static_cast<float>(kSegments);
        float x = t * width;
        float y = height * (0.5f + 0.4f * std::sin(t * 6.28318f * 2.0f + offset));
        if (s == 0)
          path.moveTo(x, y);
        else
          path.lineTo(x, y);
      }
      path.lineTo(width, height);
      path.lineTo(0, height);

      canvas.setColor(0x22000000 | (0x00ff8844 + p * 0x0400));
      canvas.fill(path);
    }
  }

  struct Scene {
    const char* name;
    SceneDraw draw;
    float blur_radius;  // 0 disables the post-effect.
  };

  constexpr Scene kScenes[] = {
    { "shapes", drawShapes, 0.0f },     { "gradients", drawGradients, 0.0f },
    { "text", drawText, 0.0f },         { "paths", drawPaths, 0.0f },
    { "blur", drawShapes, 24.0f },
  };

  struct Result {
    const char* scene = "";
    double wall_ms = 0.0;
    double cpu_ms = 0.0;
    double blocked_ms = 0.0;
    double fps = 0.0;
  };

  Result runScene(const Scene& scene, int width, int height, int frames, int warmup) {
    visage::ApplicationEditor editor;
    int frame = 0;

    editor.onDraw() = [&](visage::Canvas& canvas) {
      scene.draw(canvas, editor.width(), editor.height(), frame);
    };
    if (scene.blur_radius > 0.0f)
      editor.setBlurRadius(scene.blur_radius);

    editor.setWindowless(width, height);

    for (int i = 0; i < warmup; ++i, ++frame) {
      editor.redraw();
      editor.drawWindow();
    }
    // Drains the warmup so its cost cannot land inside the measured window.
    editor.takeScreenshot();

    double wall_start = wallSeconds();
    double cpu_start = processCpuSeconds();
    double thread_start = threadCpuSeconds();
    for (int i = 0; i < frames; ++i, ++frame) {
      editor.redraw();
      editor.drawWindow();
    }
    // The fence wait here is what makes wall time cover GPU execution.
    editor.takeScreenshot();
    double wall = wallSeconds() - wall_start;
    double cpu = processCpuSeconds() - cpu_start;
    double thread_cpu = threadCpuSeconds() - thread_start;

    Result result;
    result.scene = scene.name;
    result.wall_ms = wall * 1000.0 / frames;
    result.cpu_ms = cpu * 1000.0 / frames;
    result.blocked_ms = std::max(0.0, (wall - thread_cpu) * 1000.0 / frames);
    result.fps = frames / wall;
    return result;
  }

  int intArg(int argc, char** argv, const char* flag, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::strcmp(argv[i], flag) == 0)
        return std::atoi(argv[i + 1]);
    }
    return fallback;
  }

  bool hasArg(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
      if (std::strcmp(argv[i], flag) == 0)
        return true;
    }
    return false;
  }

  const char* stringArg(int argc, char** argv, const char* flag, const char* fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
      if (std::strcmp(argv[i], flag) == 0)
        return argv[i + 1];
    }
    return fallback;
  }
}

int main(int argc, char** argv) {
  if (hasArg(argc, argv, "--help")) {
    std::printf("usage: VisageBenchmark [--frames N] [--warmup N] [--width W] [--height H]\n"
                "                       [--scene NAME] [--csv]\n\n"
                "scenes:");
    for (const Scene& scene : kScenes)
      std::printf(" %s", scene.name);
    std::printf(" (default all)\n");
    return 0;
  }

  int frames = intArg(argc, argv, "--frames", 300);
  int warmup = intArg(argc, argv, "--warmup", 30);
  int width = intArg(argc, argv, "--width", 800);
  int height = intArg(argc, argv, "--height", 480);
  const char* only = stringArg(argc, argv, "--scene", nullptr);
  bool csv = hasArg(argc, argv, "--csv");

#if VISAGE_SDL_GPU
  visage::Renderer::instance().initializeWindowless();
#else
  // The gl backend draws through a context the application owns, so even a
  // headless run needs one; the gpu backend creates its own device.
  std::unique_ptr<visage::Window> hidden = visage::createWindow(64, 64);
  hidden->makeContextCurrent();
  visage::Renderer::instance().initialize(hidden->glProcAddressGetter());
#endif
  if (!visage::Renderer::instance().supported()) {
    std::fprintf(stderr, "renderer unavailable: %s\n",
                 visage::Renderer::instance().errorMessage().c_str());
    return 1;
  }

  const char* backend = visage::Renderer::instance().backendName();
  if (csv)
    std::printf("backend,scene,width,height,frames,wall_ms,cpu_ms,blocked_ms,fps\n");
  else {
    std::printf("backend %s, %dx%d, %d frames per scene (%d warmup)\n\n", backend, width, height,
                frames, warmup);
    std::printf("%-10s %10s %10s %10s %10s\n", "scene", "wall/ms", "cpu/ms", "blockd/ms", "fps");
  }

  bool short_run = false;
  for (const Scene& scene : kScenes) {
    if (only && std::strcmp(only, scene.name) != 0)
      continue;

    Result result = runScene(scene, width, height, frames, warmup);
    // Windows CPU clocks advance in ~15.6ms scheduler ticks, so a short run
    // reports a CPU time quantised into uselessness. POSIX is nanosecond.
    if (result.wall_ms * frames < 500.0)
      short_run = true;

    if (csv) {
      std::printf("%s,%s,%d,%d,%d,%.4f,%.4f,%.4f,%.2f\n", backend, result.scene, width, height,
                  frames, result.wall_ms, result.cpu_ms, result.blocked_ms, result.fps);
    }
    else {
      std::printf("%-10s %10.3f %10.3f %10.3f %10.1f\n", result.scene, result.wall_ms,
                  result.cpu_ms, result.blocked_ms, result.fps);
    }
    std::fflush(stdout);
  }

  if (short_run) {
    std::fprintf(stderr,
                 "warning: a scene ran under 500ms; cpu and blocked columns are quantised by "
                 "the platform's CPU clock. Raise --frames.\n");
  }

  return 0;
}
