#include "app/options.h"
// ============================================================================
// src/platform/sdl/display_sdl.cpp -- Platform::Window geometry and displays over SDL3
// ============================================================================
// Phase D.4 SDL step W4 (plan §A.3). The callers are retail transcriptions written
// against Win32 meanings (Video::Device::AdjustDisplay / SizeWindow / MoveWindow /
// GetDefaultResolution, Input::Device's clip rect, game_main's shutdown log), so each
// body here keeps the Win32 meaning its declaration in platform_window.h states. What SDL
// changes is said at each function.
//
// THE ONE ARITHMETIC THAT MATTERS: SDL positions and sizes the CLIENT area; Win32's
// SetWindowPos / GetWindowRect / AdjustWindowRect are OUTER rects. AdjustDisplay places
// the OUTER rect at the persisted Configuration::windowX / Y, which MoveWindow saved as a
// CLIENT origin (ClientToScreen of 0,0): after a user move retail's next run opens the
// window one frame-width down and right of where it was left. That drift is retail's and
// is kept by converting outer <-> client with the window's frame
// (src/platform/common/display_geometry.h), never by passing the outer values to SDL.
//
// The frame comes from SDL_GetWindowBordersSize, which on Windows measures the live
// window (GetWindowRect vs GetClientRect). It is meaningless while the window is
// minimised (an iconic window's rects) and may read 0 before the window system has
// applied a style, so the last good measurement per style (framed / not) is kept and used
// in those cases. The process is DPI-unaware (src/app/main.cpp), so a frame does not
// change size between displays.
// ============================================================================

#include "platform_window.h"
#include "display_geometry.h"
#include "window_sdl_internal.h"

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_rect.h>
#include <SDL3/SDL_video.h>

#include <cstring>

namespace Platform {
namespace Window {

namespace {

constexpr uint32_t kShowNormal    = 1;   // SW_SHOWNORMAL
constexpr uint32_t kShowMinimized = 2;   // SW_SHOWMINIMIZED
constexpr uint32_t kShowMaximized = 3;   // SW_SHOWMAXIMIZED

void Zero(Rect* r) { std::memset(r, 0, sizeof(*r)); }

// COORDINATE UNITS. SDL positions and sizes windows and displays in POINTS; the engine
// works in PIXELS (the back buffer is GetWindowSizeInPixels). On macOS with a Retina
// window (SDL_WINDOW_HIGH_PIXEL_DENSITY, OPENBA_NATIVE_RETINA) the two differ by the
// window's pixel density. Every coordinate that crosses this seam is therefore converted:
// SDL values * density going out to the engine, engine values / density going into SDL.
// Density is 1.0 on every non-Apple host and on a window without the high-density flag, so
// everything below is the identity there (display_geometry.h: PointsToPixels...).
float Density(SDL_Window* w) {
#if defined(__APPLE__)
    if (w != nullptr) {
        const float d = SDL_GetWindowPixelDensity(w);
        if (d > 0.0f) return d;
    }
#else
    (void)w;
#endif
    return 1.0f;
}

Rect FromSdl(const SDL_Rect& s, float density) {
    Rect r;
    r.left   = PointsToPixels(static_cast<float>(s.x), density);
    r.top    = PointsToPixels(static_cast<float>(s.y), density);
    r.right  = PointsToPixels(static_cast<float>(s.x + s.w), density);
    r.bottom = PointsToPixels(static_cast<float>(s.y + s.h), density);
    return r;
}

// The last good frame measurement, per style: [0] not framed, [1] framed.
FrameBorders g_frame[2]  = {};
bool         g_haveFrame[2] = {false, false};

FrameBorders CurrentFrame(SDL_Window* w) {
    const SDL_WindowFlags flags = SDL_GetWindowFlags(w);
    const int framed = (flags & SDL_WINDOW_BORDERLESS) ? 0 : 1;
    if ((flags & SDL_WINDOW_MINIMIZED) == 0) {
        int top = 0, left = 0, bottom = 0, right = 0;
        if (SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right) &&
            (framed == 0 || (top | left | bottom | right) != 0)) {
            g_frame[framed]     = FrameBorders{top, left, bottom, right};
            g_haveFrame[framed] = true;
        }
    }
    if (g_haveFrame[framed]) return g_frame[framed];
    return FrameBorders{0, 0, 0, 0};
}

// The frame in engine pixels (SDL measures it in points).
FrameBorders CurrentFramePixels(SDL_Window* w) {
    const FrameBorders f = CurrentFrame(w);
    const float d = Density(w);
    return FrameBorders{PointsToPixels(static_cast<float>(f.top), d),
                        PointsToPixels(static_cast<float>(f.left), d),
                        PointsToPixels(static_cast<float>(f.bottom), d),
                        PointsToPixels(static_cast<float>(f.right), d)};
}

}  // namespace

// GetClientRect: {0, 0, w, h}. Win32 reports an iconic window's client area as empty, and
// SizeWindow / AdjustDisplay read it back after a minimise, so a minimised window answers
// zeros here too (SDL keeps reporting the restored size).
void GetClientRect(void* /*window*/, Rect* out) {
    Zero(out);
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return;
    if (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) return;
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(w, &cw, &ch);
    out->right  = cw;
    out->bottom = ch;
}

// GetWindowRect: the client rect in screen coordinates grown by the frame.
void GetWindowRect(void* /*window*/, Rect* out) {
    Zero(out);
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return;
    int x = 0, y = 0, cw = 0, ch = 0;
    SDL_GetWindowPosition(w, &x, &y);
    SDL_GetWindowSizeInPixels(w, &cw, &ch);
    const float d = Density(w);
    out->left   = PointsToPixels(static_cast<float>(x), d);
    out->top    = PointsToPixels(static_cast<float>(y), d);
    out->right  = out->left + cw;
    out->bottom = out->top + ch;
    ClientRectToOuter(out, CurrentFramePixels(w));
}

// ClientToScreen: SDL's window position IS the client area's screen origin.
void ClientToScreen(void* /*window*/, Point* inOut) {
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return;
    int x = 0, y = 0;
    SDL_GetWindowPosition(w, &x, &y);
    // The client point arrives in pixels, the window origin is in points: the result is in
    // pixels (the engine's screen unit; see the banner above).
    const float d = Density(w);
    inOut->x += PointsToPixels(static_cast<float>(x), d);
    inOut->y += PointsToPixels(static_cast<float>(y), d);
}

uint32_t GetShowCmd(void* /*window*/) {
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return 0;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(w);
    if (flags & SDL_WINDOW_MINIMIZED) return kShowMinimized;
    if (flags & SDL_WINDOW_MAXIMIZED) return kShowMaximized;
    return kShowNormal;
}

bool IsIconic(void* /*window*/) {
    SDL_Window* w = Sdl::GameWindow();
    return w != nullptr && (SDL_GetWindowFlags(w) & SDL_WINDOW_MINIMIZED) != 0;
}

// Retail: style = (style & 0xFF30FFFF) | WS_POPUP for fullscreen, style | 0x00CF0000
// (WS_OVERLAPPEDWINDOW) for windowed, through Get/SetWindowLongW(GWL_STYLE). SDL owns the
// style bits: bordered + resizable is SDL's WS_OVERLAPPEDWINDOW (caption, system menu,
// sizing frame, min / max boxes), borderless + not resizable its WS_POPUP. One retail
// quirk is not kept: retail ORs the windowed bits onto whatever was there, so a window
// that had been fullscreen keeps WS_POPUP beside WS_OVERLAPPEDWINDOW; SDL's framed style
// has no WS_POPUP. Neither the frame nor the client area differs.
void SetFramed(void* /*window*/, bool framed) {
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return;
    SDL_SetWindowBordered(w, framed);
    SDL_SetWindowResizable(w, framed);
}

// Retail: SetWindowPos(hwnd, insertAfter, x, y, w, h, SWP_FRAMECHANGED | SWP_NOACTIVATE)
// of the OUTER rect. SDL places the client: the outer rect is converted with the frame.
// * insertAfter NoTopmost (HWND_NOTOPMOST, the windowed branch) clears always-on-top,
//   which nothing sets; Top (HWND_TOP, the fullscreen branch) raised the window in the
//   z-order WITHOUT activating it, which SDL cannot spell (SDL_RaiseWindow activates), so
//   no z-order change is made. The fullscreen branch runs while the game owns the
//   foreground in practice.
// * SDL ignores position and size requests for a MAXIMISED window (it applies them on
//   restore); retail's SetWindowPos moved the maximised window's outer origin to the
//   work-area origin and left it flagged maximised. The client size, which AdjustDisplay
//   reads back, is the same either way.
// * A size that leaves no client area (a minimised window's 0 x 0 back buffer) is refused
//   by SDL; Win32 accepted it.
void SetWindowPos(void* /*window*/, ZOrder insertAfter, int32_t x, int32_t y,
                  int32_t width, int32_t height, uint32_t /*flags*/) {
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return;
#if defined(__APPLE__)
    if (Opt().background || Opt().windowWidth > 0 || Opt().windowHeight > 0) {
        SDL_SetWindowPosition(w,8,32);SDL_SetWindowSize(w,Opt().windowWidth > 0 ? Opt().windowWidth : 640,Opt().windowHeight > 0 ? Opt().windowHeight : 360);SDL_SyncWindow(w);return;
    }
#endif
    if (insertAfter == ZOrder::NoTopmost) SDL_SetWindowAlwaysOnTop(w, false);
    // The engine's outer rect is in pixels; SDL takes points, and the frame it reports is
    // already in points.
    const float d = Density(w);
    int32_t cx = 0, cy = 0, cw = 0, ch = 0;
    OuterToClient(PixelsToPointsRound(x, d), PixelsToPointsRound(y, d),
                  PixelsToPointsRound(width, d), PixelsToPointsRound(height, d),
                  CurrentFrame(w), &cx, &cy, &cw, &ch);
    // A framed window must fit the usable screen (menu bar, Dock): the Options resolution
    // list is filtered to it, but a saved or hand-edited size may not be. Clamp the size,
    // then slide the window so title bar and bottom edge are on screen (never stuck).
    if (!(SDL_GetWindowFlags(w) & SDL_WINDOW_BORDERLESS)) {
        SDL_Rect u{0, 0, 0, 0};
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(w), &u) && u.w > 0 && u.h > 0) {
            const FrameBorders f = CurrentFrame(w);
            const int32_t maxW = u.w - f.left - f.right;
            const int32_t maxH = u.h - f.top - f.bottom;
            if (maxW > 0 && cw > maxW) cw = maxW;
            if (maxH > 0 && ch > maxH) ch = maxH;
            if (cy - f.top < u.y) cy = u.y + f.top;
            if (cy + ch + f.bottom > u.y + u.h) cy = u.y + u.h - ch - f.bottom;
            if (cx - f.left < u.x) cx = u.x + f.left;
            if (cx + cw + f.right > u.x + u.w) cx = u.x + u.w - cw - f.right;
        }
    }
    SDL_SetWindowPosition(w, cx, cy);
    SDL_SetWindowSize(w, cw, ch);
    SDL_SyncWindow(w);
}

// Retail: AdjustWindowRect(&rect, style, FALSE) with the style it had just set. Here the
// window's own current frame, which SetFramed has just set.
void AdjustWindowRect(void* /*window*/, Rect* inOut) {
    SDL_Window* w = Sdl::GameWindow();
    if (w == nullptr) return;
    SDL_SyncWindow(w);
    ClientRectToOuter(inOut, CurrentFramePixels(w));
}

// Retail: GetMonitorInfoW(Device::monitor).rcMonitor, the HMONITOR the renderer took from
// the DXGI output (devicedata.cpp). The monitor stays the DXGI output's, not the window's
// display: on Windows the SDL display carrying that HMONITOR is found by its property.
void GetMonitorRect(void* monitor, Rect* out) {
    Zero(out);
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids == nullptr) return;
    const float density = Density(Sdl::GameWindow());
    for (int i = 0; i < count; ++i) {
#if defined(SDL_PLATFORM_WINDOWS)
        void* hmon = SDL_GetPointerProperty(SDL_GetDisplayProperties(ids[i]),
                                            SDL_PROP_DISPLAY_WINDOWS_HMONITOR_POINTER,
                                            nullptr);
        if (hmon == nullptr || hmon != monitor) continue;
#else
        // dxvk-native has no HMONITOR; its DXGI output maps to the window's display.
        (void)monitor;
        SDL_Window* w = Sdl::GameWindow();
        if (w == nullptr || SDL_GetDisplayForWindow(w) != ids[i]) continue;
#endif
        SDL_Rect b;
        if (SDL_GetDisplayBounds(ids[i], &b)) *out = FromSdl(b, density);
        break;
    }
    SDL_free(ids);
}

// Retail: MonitorFromRect(&probe, MONITOR_DEFAULTTONULL) == NULL means "off every
// monitor". SDL_GetDisplayForRect never says that (it returns the closest display), so
// every display's bounds are tested for an intersection.
bool RectOnAnyDisplay(const Rect* rect) {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids == nullptr) return false;
    bool on = false;
    const float density = Density(Sdl::GameWindow());
    for (int i = 0; i < count && !on; ++i) {
        SDL_Rect b;
        if (!SDL_GetDisplayBounds(ids[i], &b)) continue;
        const Rect d = FromSdl(b, density);
        on = RectTouchesAnyDisplay(*rect, &d, 1);
    }
    SDL_free(ids);
    return on;
}

// Retail: GetWindowRect(GetDesktopWindow()), the primary monitor's rect.
void GetPrimaryDisplayRect(Rect* out) {
    Zero(out);
    SDL_Rect b;
    if (SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &b))
        *out = FromSdl(b, Density(Sdl::GameWindow()));
}

// Retail: EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS) unchecked, so a failure
// left the zeroed DEVMODEW's 0 x 0. SDL reports the mode in physical pixels from the same
// DEVMODE (WIN_GetDisplayMode).
void GetCurrentDisplaySize(int32_t* width, int32_t* height) {
    *width  = 0;
    *height = 0;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode == nullptr) return;
    // SDL_DisplayMode w / h are points; the engine's back buffer is pixels.
    const float d = Density(Sdl::GameWindow());
    *width  = PointsToPixels(static_cast<float>(mode->w), d);
    *height = PointsToPixels(static_cast<float>(mode->h), d);
}

// Was EnumDisplaySettingsW's dmDisplayFrequency (> 1, else unknown). SDL reports the rate
// as a float from the display configuration (59.94, 143.98); it is rounded to whole Hz,
// the unit DEVMODE carried.
int32_t GetCurrentDisplayRefreshHz() {
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode == nullptr || mode->refresh_rate <= 1.0f) return 0;
    return static_cast<int32_t>(mode->refresh_rate + 0.5f);
}


// ============================================================================
// The windowed OPENING geometry (the Metal backend's CreateDevice; docs/macos/INPUT_WINDOW.md
// "Opening size"). The engine's windowed back buffer starts as Configuration::windowWidth /
// Height, which is -1 until a window geometry has been saved: used as a size that collapsed
// the 10 x 10 point creation window to 1 x 1 (pixel_size_changed 20x20, then resized 1x1).
// Here the size comes from display_geometry.h's InitialWindowPoints (saved PIXELS / density,
// else the configured video size as POINTS, never below 640 x 360 points, never above the
// usable screen) and the window is placed at the saved origin when that is still on a
// display, else centred on the primary display's usable area. Everything is in points.
// ============================================================================
extern "C" void OpenBAApplyInitialWindowGeometry(SDL_Window* w, int savedX, int savedY,
                                                 int savedPxW, int savedPxH,
                                                 int videoW, int videoH) {
    if (w == nullptr) return;
    const float d = Density(w);
    SDL_Rect usable{0, 0, 0, 0};
    const bool haveUsable = SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable);
    int top = 0, left = 0, bottom = 0, right = 0;
    SDL_GetWindowBordersSize(w, &top, &left, &bottom, &right);
    if (top <= 0) top = 28;   // a hidden window may not know its frame yet: the macOS title bar
    int32_t cw = 0, ch = 0;
    InitialWindowPoints(
        savedPxW, savedPxH, videoW, videoH, d,
        haveUsable ? usable.w - left - right : 0,
        haveUsable ? usable.h - top - bottom : 0, &cw, &ch);
    fprintf(stderr, "[Window] initial geometry: saved %dx%d video %dx%d density %.1f usable %dx%d -> %dx%d pt\n", savedPxW, savedPxH, videoW, videoH, d, usable.w, usable.h, cw, ch);
    SDL_SetWindowSize(w, cw, ch);

    int px = 0, py = 0;
    bool placed = false;
    if (savedPxW > 0 && savedPxH > 0 && !(savedX == -1 && savedY == -1)) {
        px = PixelsToPointsRound(savedX, d);
        py = PixelsToPointsRound(savedY, d);
        int count = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&count);
        if (ids != nullptr) {
            for (int i = 0; i < count && !placed; ++i) {
                SDL_Rect b;
                if (!SDL_GetDisplayUsableBounds(ids[i], &b)) continue;
                // The title bar (top points above the client origin) must also be on screen.
                const Platform::Rect win{px, py - top, px + cw, py + ch};
                const Platform::Rect disp{b.x, b.y, b.x + b.w, b.y + b.h};
                placed = RectTouchesAnyDisplay(win, &disp, 1) && py - top >= b.y;
            }
            SDL_free(ids);
        }
    }
    if (!placed) {
        if (haveUsable) {
            px = usable.x + (usable.w - cw) / 2;
            py = usable.y + top + (usable.h - top - ch) / 2;
        } else {
            px = 100;
            py = 100;
        }
    }
    SDL_SetWindowPosition(w, px, py);
    SDL_SyncWindow(w);
}


// The primary display as the Options resolution list needs it, in engine PIXELS: the usable
// area less a 32-point title bar (maxW/maxH) and the display's own size (nativeW/nativeH).
// All zero when SDL cannot say (BuildResolutionList then does not filter).
extern "C" void OpenBAQueryScreenForModes(int* maxW, int* maxH, int* nativeW, int* nativeH) {
    *maxW = *maxH = *nativeW = *nativeH = 0;
    const SDL_DisplayID id = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(id);
    float d = 1.0f;
#if defined(__APPLE__)
    if (mode != nullptr && mode->pixel_density > 0.0f) d = mode->pixel_density;
#endif
    SDL_Rect u{0, 0, 0, 0};
    if (SDL_GetDisplayUsableBounds(id, &u) && u.w > 0 && u.h > 0) {
        *maxW = PointsToPixels(static_cast<float>(u.w), d);
        *maxH = PointsToPixels(static_cast<float>(u.h - 32), d);
    }
    if (mode != nullptr) {
        *nativeW = PointsToPixels(static_cast<float>(mode->w), d);
        *nativeH = PointsToPixels(static_cast<float>(mode->h), d);
    }
}

}  // namespace Window
}  // namespace Platform
