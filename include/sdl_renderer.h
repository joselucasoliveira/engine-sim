#ifndef ENGINE_SIM_SDL_RENDERER_H
#define ENGINE_SIM_SDL_RENDERER_H
#include "desktop_host.h"
#include "font_atlas.h"
#include <memory>

struct SDL_Window;
namespace desktop {
class SdlRenderer {
public:
    struct Statistics {
        std::uint64_t submittedFrames = 0, presentedFrames = 0, draws = 0, glyphs = 0;
        int width = 0, height = 0;
        std::string driver;
    };
    SdlRenderer();
    ~SdlRenderer();
    SdlRenderer(const SdlRenderer &) = delete;
    SdlRenderer &operator=(const SdlRenderer &) = delete;
    void initialize(SDL_Window *window, const engine_sim::ResourcePaths &resources,
                    const engine_sim::CompiledScene &scene);
    void shutdown() noexcept;
    void uploadGeometry(const Vertex *vertices, std::size_t vertexCount,
                        const std::uint16_t *indices, std::size_t indexCount);
    void render(const Host &host, const FrameState &frame, bool present = true);
    std::vector<std::uint8_t> readPixels(); // RGBA8, top-left physical pixels, last completed frame.
    float measureText(const std::string &text, float height) const;
    const Statistics &statistics() const;
private:
    struct State;
    std::unique_ptr<State> state;
};
}
#endif
