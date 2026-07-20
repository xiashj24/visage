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

#include "windowing_sdl3.h"

#include <map>
#include <SDL3/SDL.h>
#include <vector>

namespace visage {
  namespace {
    // One process-wide GL context shared by every window, so a single
    // Renderer can draw all of them.
    SDL_GLContext g_gl_context = nullptr;
    std::map<SDL_WindowID, WindowSdl3*> g_windows;
    bool g_quit_event_loop = false;
    std::vector<std::string> g_drag_files;

    void setGlContextAttributes() {
#if VISAGE_OPENGL_ES
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#elif defined(__APPLE__)
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
      SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
      SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
      SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    }

    void ensureSdlInitialized() {
      static bool initialized = [] {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
          VISAGE_LOG(SDL_GetError());
          VISAGE_ASSERT(false);
          return false;
        }
        return true;
      }();
      (void)initialized;
    }

    // Pixels per window-coordinate unit of the primary display: 1.0 on
    // Windows/KMSDRM, the retina factor on macOS.
    float primaryDisplayPixelDensity() {
      const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
      return mode && mode->pixel_density > 0.0f ? mode->pixel_density : 1.0f;
    }

    int currentModifiers() {
      SDL_Keymod state = SDL_GetModState();
      int modifiers = 0;
      if (state & SDL_KMOD_SHIFT)
        modifiers |= kModifierShift;
      if (state & SDL_KMOD_CTRL)
#if defined(__APPLE__)
        modifiers |= kModifierMacCtrl;
#else
        modifiers |= kModifierRegCtrl;
#endif
      if (state & SDL_KMOD_ALT)
        modifiers |= kModifierAlt;
      if (state & SDL_KMOD_GUI)
#if defined(__APPLE__)
        modifiers |= kModifierCmd;
#else
        modifiers |= kModifierMeta;
#endif
      return modifiers;
    }

    int mouseButtonStateFromSdl(Uint32 sdl_state) {
      int state = 0;
      if (sdl_state & SDL_BUTTON_LMASK)
        state |= kMouseButtonLeft;
      if (sdl_state & SDL_BUTTON_MMASK)
        state |= kMouseButtonMiddle;
      if (sdl_state & SDL_BUTTON_RMASK)
        state |= kMouseButtonRight;
      return state;
    }

    MouseButton mouseButtonFromSdl(Uint8 button) {
      switch (button) {
      case SDL_BUTTON_LEFT: return kMouseButtonLeft;
      case SDL_BUTTON_MIDDLE: return kMouseButtonMiddle;
      case SDL_BUTTON_RIGHT: return kMouseButtonRight;
      default: return kMouseButtonNone;
      }
    }

    // visage's KeyCode values are SDL keycodes for printable keys and
    // SDL scancode - 56 behind the unprintable mask - translation is direct.
    KeyCode keyCodeFromSdl(SDL_Keycode key, SDL_Scancode scancode) {
      static constexpr int kFirstUnprintableScancode = SDL_SCANCODE_CAPSLOCK;

      if (key == SDLK_RETURN)
        return KeyCode::Return;
      if ((key & SDLK_SCANCODE_MASK) == 0 && key < 128)
        return static_cast<KeyCode>(key);
      if (scancode >= kFirstUnprintableScancode)
        return static_cast<KeyCode>(kUnprintableKeycodeMask | (scancode - kFirstUnprintableScancode + 1));
      return KeyCode::Unknown;
    }

    WindowSdl3* windowFromId(SDL_WindowID id) {
      auto it = g_windows.find(id);
      return it == g_windows.end() ? nullptr : it->second;
    }

    SDL_WindowID eventWindowId(const SDL_Event& event) {
      // The window id lives at the same offset for all window-targeted events.
      switch (event.type) {
      case SDL_EVENT_MOUSE_MOTION: return event.motion.windowID;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: return event.button.windowID;
      case SDL_EVENT_MOUSE_WHEEL: return event.wheel.windowID;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP: return event.key.windowID;
      case SDL_EVENT_TEXT_INPUT: return event.text.windowID;
      case SDL_EVENT_DROP_BEGIN:
      case SDL_EVENT_DROP_FILE:
      case SDL_EVENT_DROP_POSITION:
      case SDL_EVENT_DROP_COMPLETE: return event.drop.windowID;
      default:
        if (event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST)
          return event.window.windowID;
        return 0;
      }
    }

    void dispatchEvent(const SDL_Event& event) {
      if (event.type == SDL_EVENT_QUIT) {
        g_quit_event_loop = true;
        return;
      }

      if (WindowSdl3* window = windowFromId(eventWindowId(event)))
        window->processEvent(event);
    }
  }

  WindowSdl3::WindowSdl3(int x, int y, int width, int height, Decoration decoration) :
      Window(width, height), owns_window_(true) {
    ensureSdlInitialized();
    setGlContextAttributes();

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE |
                            SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (decoration == Decoration::Client || decoration == Decoration::Popup)
      flags |= SDL_WINDOW_BORDERLESS;

    // Requested size is physical pixels; SDL takes window coordinates.
    float density = primaryDisplayPixelDensity();
    int window_width = static_cast<int>(std::round(width / density));
    int window_height = static_cast<int>(std::round(height / density));

    window_ = SDL_CreateWindow("", window_width, window_height, flags);
    if (window_ == nullptr) {
      VISAGE_LOG(SDL_GetError());
      VISAGE_ASSERT(false);
      return;
    }

    SDL_SetWindowPosition(window_, x, y);
    initialize();
  }

  WindowSdl3::WindowSdl3(SDL_Window* existing_window) : Window(), owns_window_(false) {
    ensureSdlInitialized();
    window_ = existing_window;
    VISAGE_ASSERT(window_);
    if (window_)
      initialize();
  }

  void WindowSdl3::initialize() {
    if (g_gl_context == nullptr) {
      g_gl_context = SDL_GL_CreateContext(window_);
      if (g_gl_context == nullptr) {
        VISAGE_LOG(SDL_GetError());
        VISAGE_ASSERT(false);
      }
    }

    makeContextCurrent();
    SDL_GL_SetSwapInterval(1);

    g_windows[SDL_GetWindowID(window_)] = this;
    updateDpiScale();
    updateClientSize();
  }

  WindowSdl3::~WindowSdl3() {
    close();
  }

  void* WindowSdl3::nativeHandle() const {
    // Never null while the window exists - other layers use its truthiness
    // to identify a window-attached (vs offscreen) render target.
    return window_;
  }

  Window::GlProcAddressGetter WindowSdl3::glProcAddressGetter() const {
    return reinterpret_cast<GlProcAddressGetter>(SDL_GL_GetProcAddress);
  }

  void WindowSdl3::makeContextCurrent() {
    if (window_ && g_gl_context)
      SDL_GL_MakeCurrent(window_, g_gl_context);
  }

  void WindowSdl3::swapBuffers() {
    if (window_)
      SDL_GL_SwapWindow(window_);
  }

  void WindowSdl3::updateDpiScale() {
    float scale = SDL_GetWindowDisplayScale(window_);
    if (scale > 0.0f)
      setDpiScale(scale);
  }

  void WindowSdl3::updateClientSize() {
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    if (screenRotationQuarterTurns(screenRotation()) & 1)
      std::swap(width, height);
    handleResized(width, height);
  }

  // Maps physical window coordinates to the UI's logical, unrotated space.
  Point WindowSdl3::physicalToLogical(float x, float y) const {
    float density = SDL_GetWindowPixelDensity(window_);
    float px = x * density;
    float py = y * density;

    int physical_width = 0, physical_height = 0;
    SDL_GetWindowSizeInPixels(window_, &physical_width, &physical_height);

    switch (screenRotationQuarterTurns(screenRotation())) {
    case 1: return { py, physical_width - 1 - px };
    case 2: return { physical_width - 1 - px, physical_height - 1 - py };
    case 3: return { physical_height - 1 - py, px };
    default: return { px, py };
    }
  }

  void WindowSdl3::checkTextInput() {
    bool wants_text = hasActiveTextEntry();
    bool active = SDL_TextInputActive(window_);
    if (wants_text && !active)
      SDL_StartTextInput(window_);
    else if (!wants_text && active)
      SDL_StopTextInput(window_);
  }

  void WindowSdl3::processEvent(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
      int button_state = mouseButtonStateFromSdl(event.motion.state);
      if (mouseRelativeMode()) {
        float density = SDL_GetWindowPixelDensity(window_);
        handleMouseMove(static_cast<int>(std::round(event.motion.xrel * density)),
                        static_cast<int>(std::round(event.motion.yrel * density)), button_state,
                        currentModifiers());
      }
      else {
        Point point = physicalToLogical(event.motion.x, event.motion.y);
        handleMouseMove(static_cast<int>(std::round(point.x)),
                        static_cast<int>(std::round(point.y)), button_state, currentModifiers());
      }
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      Point point = physicalToLogical(event.button.x, event.button.y);
      int x = static_cast<int>(std::round(point.x));
      int y = static_cast<int>(std::round(point.y));
      MouseButton button = mouseButtonFromSdl(event.button.button);
      int button_state = mouseButtonStateFromSdl(SDL_GetMouseState(nullptr, nullptr));
      if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
        handleMouseDown(button, x, y, button_state, currentModifiers());
      else
        handleMouseUp(button, x, y, button_state, currentModifiers());
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      Point point = physicalToLogical(event.wheel.mouse_x, event.wheel.mouse_y);
      int button_state = mouseButtonStateFromSdl(SDL_GetMouseState(nullptr, nullptr));
      handleMouseWheel(event.wheel.x, event.wheel.y, event.wheel.x, event.wheel.y,
                       static_cast<int>(std::round(point.x)), static_cast<int>(std::round(point.y)),
                       button_state, currentModifiers());
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER: {
      float x = 0.0f, y = 0.0f;
      SDL_GetMouseState(&x, &y);
      Point point = physicalToLogical(x, y);
      handleMouseEnter(static_cast<int>(std::round(point.x)), static_cast<int>(std::round(point.y)));
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
      handleMouseLeave(mouseButtonStateFromSdl(SDL_GetMouseState(nullptr, nullptr)), currentModifiers());
      break;
    case SDL_EVENT_KEY_DOWN: {
      KeyCode key_code = keyCodeFromSdl(event.key.key, event.key.scancode);
      handleKeyDown(key_code, currentModifiers(), event.key.repeat);
      break;
    }
    case SDL_EVENT_KEY_UP:
      handleKeyUp(keyCodeFromSdl(event.key.key, event.key.scancode), currentModifiers());
      break;
    case SDL_EVENT_TEXT_INPUT:
      handleTextInput(event.text.text);
      break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      updateClientSize();
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      updateDpiScale();
      updateClientSize();
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      handleFocusGained();
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      handleFocusLost();
      break;
    case SDL_EVENT_WINDOW_SHOWN:
      setVisible(true);
      handleWindowShown();
      break;
    case SDL_EVENT_WINDOW_HIDDEN:
      setVisible(false);
      handleWindowHidden();
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      if (handleCloseRequested())
        close();
      break;
    case SDL_EVENT_DROP_BEGIN:
      g_drag_files.clear();
      break;
    case SDL_EVENT_DROP_POSITION: {
      Point point = physicalToLogical(event.drop.x, event.drop.y);
      // SDL doesn't expose the file list until the drop completes; an empty
      // placeholder still lets targets show drag feedback.
      std::vector<std::string> files = g_drag_files.empty() ? std::vector<std::string> { "" } :
                                                              g_drag_files;
      handleFileDrag(static_cast<int>(std::round(point.x)), static_cast<int>(std::round(point.y)), files);
      break;
    }
    case SDL_EVENT_DROP_FILE:
      if (event.drop.data)
        g_drag_files.push_back(event.drop.data);
      break;
    case SDL_EVENT_DROP_COMPLETE: {
      Point point = physicalToLogical(event.drop.x, event.drop.y);
      handleFileDrop(static_cast<int>(std::round(point.x)), static_cast<int>(std::round(point.y)),
                     g_drag_files);
      g_drag_files.clear();
      break;
    }
    default:
      break;
    }
  }

  void WindowSdl3::runFrame(double time) {
    makeContextCurrent();
    checkTextInput();
    drawCallback(time);
    swapBuffers();
  }

  void WindowSdl3::runEventLoop() {
    g_quit_event_loop = false;

    SDL_Event event;
    while (!g_quit_event_loop && !g_windows.empty()) {
      while (SDL_PollEvent(&event))
        dispatchEvent(event);

      if (g_quit_event_loop || g_windows.empty())
        break;

      bool any_visible = false;
      std::vector<WindowSdl3*> windows;
      windows.reserve(g_windows.size());
      for (auto& [id, window] : g_windows)
        windows.push_back(window);

      double time = SDL_GetTicks() / 1000.0;
      for (WindowSdl3* window : windows) {
        if (window->isShowing()) {
          any_visible = true;
          window->runFrame(time);
        }
      }

      // Vsync paces the loop while something is on screen.
      if (!any_visible)
        SDL_WaitEventTimeout(nullptr, 10);
    }
  }

  void WindowSdl3::windowContentsResized(int width, int height) {
    if (window_ == nullptr)
      return;

    if (screenRotationQuarterTurns(screenRotation()) & 1)
      std::swap(width, height);
    float density = SDL_GetWindowPixelDensity(window_);
    SDL_SetWindowSize(window_, static_cast<int>(std::round(width / density)),
                      static_cast<int>(std::round(height / density)));
  }

  void WindowSdl3::show() {
    if (window_) {
      SDL_ShowWindow(window_);
      setVisible(true);
    }
  }

  void WindowSdl3::showMaximized() {
    if (window_) {
      SDL_MaximizeWindow(window_);
      SDL_ShowWindow(window_);
      setVisible(true);
    }
  }

  void WindowSdl3::hide() {
    if (window_) {
      SDL_HideWindow(window_);
      setVisible(false);
    }
  }

  void WindowSdl3::close() {
    if (window_ == nullptr)
      return;

    g_windows.erase(SDL_GetWindowID(window_));
    if (owns_window_)
      SDL_DestroyWindow(window_);
    window_ = nullptr;
  }

  bool WindowSdl3::isShowing() const {
    return window_ && isVisible();
  }

  void WindowSdl3::setWindowTitle(const std::string& title) {
    if (window_)
      SDL_SetWindowTitle(window_, title.c_str());
  }

  IPoint WindowSdl3::maxWindowDimensions() const {
    SDL_Rect bounds {};
    SDL_DisplayID display = SDL_GetDisplayForWindow(window_);
    if (display == 0)
      display = SDL_GetPrimaryDisplay();
    SDL_GetDisplayUsableBounds(display, &bounds);
    float density = SDL_GetWindowPixelDensity(window_);
    return { static_cast<int>(bounds.w * density), static_cast<int>(bounds.h * density) };
  }

  void WindowSdl3::setFixedAspectRatio(bool fixed) {
    fixed_aspect_ratio_ = fixed;
    if (window_ == nullptr)
      return;

    if (fixed) {
      int width = 0, height = 0;
      SDL_GetWindowSize(window_, &width, &height);
      aspect_ratio_ = height ? width / static_cast<float>(height) : 1.0f;
      SDL_SetWindowAspectRatio(window_, aspect_ratio_, aspect_ratio_);
    }
    else
      SDL_SetWindowAspectRatio(window_, 0.0f, 0.0f);
  }

  void WindowSdl3::setAlwaysOnTop(bool on_top) {
    if (window_)
      SDL_SetWindowAlwaysOnTop(window_, on_top);
  }

  void setCursorStyle(MouseCursor style) {
    static std::map<SDL_SystemCursor, SDL_Cursor*> cursors;

    if (style == MouseCursor::Invisible) {
      SDL_HideCursor();
      return;
    }

    SDL_SystemCursor system_cursor = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (style) {
    case MouseCursor::Arrow: system_cursor = SDL_SYSTEM_CURSOR_DEFAULT; break;
    case MouseCursor::IBeam: system_cursor = SDL_SYSTEM_CURSOR_TEXT; break;
    case MouseCursor::Crosshair: system_cursor = SDL_SYSTEM_CURSOR_CROSSHAIR; break;
    case MouseCursor::Pointing: system_cursor = SDL_SYSTEM_CURSOR_POINTER; break;
    case MouseCursor::Dragging: system_cursor = SDL_SYSTEM_CURSOR_MOVE; break;
    case MouseCursor::HorizontalResize: system_cursor = SDL_SYSTEM_CURSOR_EW_RESIZE; break;
    case MouseCursor::VerticalResize: system_cursor = SDL_SYSTEM_CURSOR_NS_RESIZE; break;
    case MouseCursor::TopLeftResize:
    case MouseCursor::BottomRightResize: system_cursor = SDL_SYSTEM_CURSOR_NWSE_RESIZE; break;
    case MouseCursor::TopRightResize:
    case MouseCursor::BottomLeftResize: system_cursor = SDL_SYSTEM_CURSOR_NESW_RESIZE; break;
    case MouseCursor::MultiDirectionalResize: system_cursor = SDL_SYSTEM_CURSOR_MOVE; break;
    default: break;
    }

    if (cursors.count(system_cursor) == 0)
      cursors[system_cursor] = SDL_CreateSystemCursor(system_cursor);
    SDL_SetCursor(cursors[system_cursor]);
    SDL_ShowCursor();
  }

  void setCursorVisible(bool visible) {
    if (visible)
      SDL_ShowCursor();
    else
      SDL_HideCursor();
  }

  Point cursorPosition() {
    float x = 0.0f, y = 0.0f;
    SDL_GetMouseState(&x, &y);
    SDL_Window* focus = SDL_GetMouseFocus();
    if (focus) {
      if (WindowSdl3* window = windowFromId(SDL_GetWindowID(focus)))
        return window->physicalToLogical(x, y);
    }
    return { x, y };
  }

  void setCursorPosition(Point window_position) {
    SDL_Window* focus = SDL_GetMouseFocus();
    if (focus == nullptr)
      return;
    float density = SDL_GetWindowPixelDensity(focus);
    SDL_WarpMouseInWindow(focus, window_position.x / density, window_position.y / density);
  }

  void setCursorScreenPosition(Point screen_position) {
    SDL_WarpMouseGlobal(screen_position.x, screen_position.y);
  }

  bool isMobileDevice() {
    return false;
  }

  void showMessageBox(std::string title, std::string message) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title.c_str(), message.c_str(), nullptr);
  }

  std::string readClipboardText() {
    char* text = SDL_GetClipboardText();
    std::string result = text ? text : "";
    SDL_free(text);
    return result;
  }

  void setClipboardText(const std::string& text) {
    SDL_SetClipboardText(text.c_str());
  }

  float defaultDpiScale() {
    ensureSdlInitialized();
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return scale > 0.0f ? scale : 1.0f;
  }

  IBounds computeWindowBounds(const Dimension& x, const Dimension& y, const Dimension& width,
                              const Dimension& height) {
    ensureSdlInitialized();

    float scale = defaultDpiScale();
    SDL_Rect usable {};
    SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    // Dimensions compute in physical pixels; usable bounds are in window
    // coordinates.
    float density = primaryDisplayPixelDensity();
    float screen_width = usable.w * density;
    float screen_height = usable.h * density;

    int w = width.computeInt(scale, screen_width, screen_height, screen_width / 2);
    int h = height.computeInt(scale, screen_width, screen_height, screen_height / 2);
    int default_x = (screen_width - w) / 2;
    int default_y = (screen_height - h) / 2;
    return { x.computeInt(scale, screen_width, screen_height, default_x),
             y.computeInt(scale, screen_width, screen_height, default_y), w, h };
  }

  std::unique_ptr<Window> createWindow(const Dimension& x, const Dimension& y,
                                       const Dimension& width, const Dimension& height,
                                       Window::Decoration decoration_style) {
    IBounds bounds = computeWindowBounds(x, y, width, height);
    return std::make_unique<WindowSdl3>(bounds.x(), bounds.y(), bounds.width(), bounds.height(),
                                        decoration_style);
  }

  std::unique_ptr<Window> createPluginWindow(const Dimension& width, const Dimension& height,
                                             void* parent_handle) {
    // Plugin-editor embedding (attaching into a host-owned native window) is
    // out of scope for the SDL3 port.
    VISAGE_LOG("createPluginWindow is not supported by the SDL3 windowing backend");
    return nullptr;
  }

  void closeApplication() {
    g_quit_event_loop = true;
  }
}
