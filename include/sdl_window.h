#ifndef ENGINE_SIM_SDL_WINDOW_H
#define ENGINE_SIM_SDL_WINDOW_H

#include "desktop_host.h"
#include <memory>

struct SDL_Window;
namespace desktop {
// Internal video/event owner. Rendering and audio attach separately on the same thread.
class SdlWindow {
public:
    struct Options {
        std::string title = "Engine Simulator (independent fork)";
        int width = 1920, height = 1080;
        bool hidden = false;
    };
    SdlWindow();
    ~SdlWindow();
    SdlWindow(const SdlWindow &) = delete;
    SdlWindow &operator=(const SdlWindow &) = delete;
    void initialize(const Options &options);
    void shutdown() noexcept;
    void pumpEvents();
    const FrameState &frame() const;
    bool isKeyDown(Key key) const;
    bool processKeyDown(Key key);
    bool processMouseDown(Button button);
    bool processMouseUp(Button button);
    int mouseWheel() const;
    void mousePosition(int *x, int *y) const;
    void setFullscreen(bool enabled);
    SDL_Window *nativeHandle() const;
    std::string videoDriver() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
#endif
