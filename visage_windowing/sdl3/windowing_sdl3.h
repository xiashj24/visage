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

#include "windowing.h"

struct SDL_Window;
union SDL_Event;

namespace visage {
  // SDL3 windowing for every platform (Win32/Cocoa/KMSDRM behind SDL).
  //
  // All windows share one GL context, created with the first window and made
  // current per window before drawing - this is what lets a single Renderer
  // serve multiple windows. When the global screen rotation is a quarter
  // turn, the logical client size reported to visage is the physical drawable
  // size with the axes swapped, and mouse input is mapped physical->logical;
  // the rotation itself happens in the graphics present pass.
  class WindowSdl3 : public Window {
  public:
    // Creates and owns an SDL window sized in physical pixels.
    WindowSdl3(int x, int y, int width, int height, Decoration decoration);
    // Wraps an app-created SDL window without taking ownership. The window
    // must have been created with the OpenGL flag.
    explicit WindowSdl3(SDL_Window* existing_window);
    ~WindowSdl3() override;

    void runEventLoop() override;
    void* nativeHandle() const override;
    void windowContentsResized(int width, int height) override;

    GlProcAddressGetter glProcAddressGetter() const override;
    void makeContextCurrent() override;
    void swapBuffers() override;

    void show() override;
    void showMaximized() override;
    void hide() override;
    void close() override;
    bool isShowing() const override;
    void setWindowTitle(const std::string& title) override;
    IPoint maxWindowDimensions() const override;
    void setFixedAspectRatio(bool fixed) override;
    void setAlwaysOnTop(bool on_top) override;

    // For applications running their own SDL event loop: translate one event
    // and drive one frame (draw callback + buffer swap).
    void processEvent(const SDL_Event& event);
    void runFrame(double time);

    SDL_Window* sdlWindow() const { return window_; }

    // Maps physical window coordinates to the UI's logical, unrotated space.
    Point physicalToLogical(float x, float y) const;

  private:
    void initialize();
    void updateClientSize();
    void updateDpiScale();
    void checkTextInput();
    bool handleDecorationClick(bool mouse_down, int x, int y);

    SDL_Window* window_ = nullptr;
    bool owns_window_ = false;
    Decoration decoration_ = Decoration::Native;
    HitTestResult pressed_decoration_button_ = HitTestResult::Client;
    bool fixed_aspect_ratio_ = false;
    float aspect_ratio_ = 1.0f;
  };
}
