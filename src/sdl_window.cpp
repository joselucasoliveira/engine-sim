#include "../include/sdl_window.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

namespace desktop {
namespace {
// Physical positions preserve the application's existing key table, independent of layout.
constexpr std::array<SDL_Scancode, static_cast<std::size_t>(Key::Count)> keys{{
    SDL_SCANCODE_ESCAPE, SDL_SCANCODE_RETURN, SDL_SCANCODE_TAB, SDL_SCANCODE_F, SDL_SCANCODE_INSERT,
    SDL_SCANCODE_RIGHT, SDL_SCANCODE_SPACE, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_Z, SDL_SCANCODE_X,
    SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_N, SDL_SCANCODE_G,
    SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R, SDL_SCANCODE_M,
    SDL_SCANCODE_COMMA, SDL_SCANCODE_D, SDL_SCANCODE_H, SDL_SCANCODE_S, SDL_SCANCODE_A,
    SDL_SCANCODE_T, SDL_SCANCODE_U, SDL_SCANCODE_Y, SDL_SCANCODE_I
}};
void checked(bool success, const char *operation) {
    if (!success) throw DesktopError(operation, SDL_GetError());
}
}
struct SdlWindow::State {
    const std::thread::id owner = std::this_thread::get_id();
    SDL_Window *window = nullptr;
    FrameState frame;
    std::array<bool, SDL_SCANCODE_COUNT> physical{};
    std::array<bool, keys.size()> held{}, pressed{};
    bool mouseHeld = false, mouseDown = false, mouseUp = false, videoOwned = false;
    float mouseX = 0, mouseY = 0;
    double wheel = 0;
    std::uint64_t previous = 0;
    std::array<float, 32> intervals{};
    std::size_t intervalIndex = 0, intervalCount = 0;
    void requireOwner() const {
        if (owner != std::this_thread::get_id()) throw DesktopError("window", "video and events require their owning thread");
    }
    void clearInput() {
        physical.fill(false); held.fill(false); pressed.fill(false);
        mouseHeld = mouseDown = mouseUp = false;
    }
    void size() {
        auto &w = frame.window;
        checked(SDL_GetWindowSize(window, &w.gameWidth, &w.gameHeight), "logical window size");
        checked(SDL_GetWindowSizeInPixels(window, &w.framebufferWidth, &w.framebufferHeight), "framebuffer size");
        w.screenWidth = w.gameWidth; w.screenHeight = w.gameHeight;
    }
    void mouse(float x, float y) {
        if (std::isfinite(x) && std::isfinite(y)) { mouseX = x; mouseY = y; }
    }
};
SdlWindow::SdlWindow() : state(std::make_unique<State>()) { state->frame.window.open = false; }
SdlWindow::~SdlWindow() { shutdown(); }
void SdlWindow::initialize(const Options &options) {
    state->requireOwner();
    if (state->window || state->videoOwned) throw DesktopError("initialize", "window is already initialized");
    if (options.width <= 0 || options.height <= 0) throw DesktopError("initialize", "window dimensions must be positive");
#ifdef __APPLE__
    if (!SDL_IsMainThread()) throw DesktopError("initialize", "macOS video requires the main thread");
#endif
    checked(SDL_InitSubSystem(SDL_INIT_VIDEO), "video initialization");
    state->videoOwned = true;
    try {
        auto flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (options.hidden) flags |= SDL_WINDOW_HIDDEN;
        state->window = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
        if (!state->window) throw DesktopError("create window", SDL_GetError());
        state->frame = {};
        state->frame.window.open = true;
        state->frame.window.active = (SDL_GetWindowFlags(state->window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        state->size(); state->clearInput(); state->wheel = 0;
        state->previous = SDL_GetTicksNS(); state->frame.monotonicNanoseconds = state->previous;
        state->intervals.fill(0); state->intervalCount = state->intervalIndex = 0;
    } catch (...) { shutdown(); throw; }
}
void SdlWindow::shutdown() noexcept {
    // Destroying a video object on another thread violates SDL/Cocoa ownership.
    if (state->owner != std::this_thread::get_id()) std::terminate();
    if (state->window) { SDL_DestroyWindow(state->window); state->window = nullptr; }
    if (state->videoOwned) { SDL_QuitSubSystem(SDL_INIT_VIDEO); state->videoOwned = false; }
    state->clearInput(); state->frame.window.open = state->frame.window.active = false;
}
void SdlWindow::pumpEvents() {
    state->requireOwner();
    if (!state->window) throw DesktopError("events", "window is not initialized");
    state->pressed.fill(false); state->mouseDown = state->mouseUp = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) { state->frame.window.open = false; state->clearInput(); continue; }
        if (SDL_GetWindowFromEvent(&event) != state->window) continue;
        switch (event.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            state->frame.window.open = false; state->clearInput(); break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: state->frame.window.active = true; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            state->frame.window.active = false; state->clearInput(); break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            // Query both dimensions: SDL event payloads describe only one coordinate space.
            state->size(); break;
        case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: state->frame.window.fullscreen = true; state->size(); break;
        case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: state->frame.window.fullscreen = false; state->size(); break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            const auto scan = event.key.scancode;
            if (!state->frame.window.active || scan < 0 || scan >= SDL_SCANCODE_COUNT) break;
            state->physical[scan] = event.type == SDL_EVENT_KEY_DOWN;
            for (std::size_t i = 0; i < keys.size(); ++i) {
                const bool down = state->physical[keys[i]] || (static_cast<Key>(i) == Key::Shift && state->physical[SDL_SCANCODE_RSHIFT]);
                if (down && !state->held[i] && !event.key.repeat) state->pressed[i] = true;
                state->held[i] = down;
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: state->mouse(event.motion.x, event.motion.y); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            state->mouse(event.button.x, event.button.y);
            if (!state->frame.window.active || event.button.button != SDL_BUTTON_LEFT) break;
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (!state->mouseHeld) state->mouseDown = true;
                state->mouseHeld = true;
            } else { state->mouseHeld = false; state->mouseUp = true; }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (state->frame.window.active && std::isfinite(event.wheel.y)) {
                const double delta = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                // Windows raw-input wheel units are 120 per detent. Retain fractional SDL motion.
                constexpr double limit = std::numeric_limits<int>::max() / 4;
                state->wheel = std::clamp(state->wheel + delta * 120.0, -limit, limit);
            }
            break;
        default: break;
        }
    }
    const auto now = SDL_GetTicksNS();
    state->frame.seconds = static_cast<float>((now - state->previous) / 1.0e9);
    state->frame.monotonicNanoseconds = now; state->previous = now;
    if (state->frame.seconds > 0) {
        state->intervals[state->intervalIndex] = state->frame.seconds;
        state->intervalIndex = (state->intervalIndex + 1) % state->intervals.size();
        state->intervalCount = std::min(state->intervalCount + 1, state->intervals.size());
        double total = 0; for (std::size_t i = 0; i < state->intervalCount; ++i) total += state->intervals[i];
        state->frame.averageFps = static_cast<float>(state->intervalCount / total);
    }
}
const FrameState &SdlWindow::frame() const { state->requireOwner(); return state->frame; }
bool SdlWindow::isKeyDown(Key key) const { state->requireOwner(); return state->held.at(static_cast<std::size_t>(key)); }
bool SdlWindow::processKeyDown(Key key) { state->requireOwner(); return std::exchange(state->pressed.at(static_cast<std::size_t>(key)), false); }
bool SdlWindow::processMouseDown(Button button) {
    state->requireOwner(); return button == Button::Left && state->frame.window.active && std::exchange(state->mouseDown, false);
}
bool SdlWindow::processMouseUp(Button button) {
    state->requireOwner(); return button == Button::Left && state->frame.window.active && std::exchange(state->mouseUp, false);
}
int SdlWindow::mouseWheel() const { state->requireOwner(); return static_cast<int>(state->wheel); }
void SdlWindow::mousePosition(int *x, int *y) const {
    state->requireOwner();
    if (x) *x = static_cast<int>(state->mouseX);
    if (y) *y = state->frame.window.gameHeight - static_cast<int>(state->mouseY);
}
void SdlWindow::setFullscreen(bool enabled) {
    state->requireOwner();
    if (!state->window) throw DesktopError("fullscreen", "window is not initialized");
    checked(SDL_SetWindowFullscreen(state->window, enabled), "fullscreen");
    state->frame.window.fullscreen = enabled;
}
SDL_Window *SdlWindow::nativeHandle() const { state->requireOwner(); return state->window; }
std::string SdlWindow::videoDriver() const {
    state->requireOwner(); const auto *driver = SDL_GetCurrentVideoDriver(); return driver ? driver : "";
}
}
