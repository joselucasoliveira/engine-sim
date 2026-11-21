#ifndef ENGINE_SIM_DESKTOP_HOST_H
#define ENGINE_SIM_DESKTOP_HOST_H

#include "desktop_math.h"
#include "compiled_scene.h"
#include "resource_paths.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace desktop {
enum class Key {
    Escape, Return, Tab, F, Insert, Right, Space, Shift, Up, Down,
    N1, N2, N3, N4, N5, F1, F2, F3, Z, X, C, V, B, N, G,
    Q, W, E, R, M, OEM_Comma, D, H, S, A, T, U, Y, I, Count
};
enum class Button { Left, Count };
enum class WindowStyle { Windowed, Fullscreen };
using StageEnableFlags = std::uint64_t;
inline constexpr StageEnableFlags SceneStage = 1, UiStage = 2, TextStage = 4;
using AudioOffset = std::uint32_t;

struct Vertex { Vector4 Pos; Vector2 TexCoord; Vector4 Normal; };
static_assert(sizeof(Vertex) == 40, "The scene and generated geometry share the original 40-byte layout");
struct Model { std::string name; std::size_t faceCount = 0; };
struct Viewport { int x = 0, y = 0, width = 0, height = 0; };
struct WindowState {
    int gameWidth = 1920, gameHeight = 1080;
    int screenWidth = 1920, screenHeight = 1080;
    int framebufferWidth = 1920, framebufferHeight = 1080;
    bool active = true, open = true, fullscreen = false;
};
struct FrameState {
    WindowState window;
    float seconds = 0, averageFps = 60;
    std::uint64_t monotonicNanoseconds = 0;
};
struct Camera {
    Matrix view = math::LoadIdentity(), projection = math::LoadIdentity();
    Vector eye;
};
struct DrawingState {
    Camera scene, ui;
    Matrix transform = math::LoadIdentity();
    Vector color{1,1,1,1}, clearColor{0,0,0,1};
    Viewport viewport;
};
struct DrawPacket {
    enum class Kind { Model, Generated, Text } kind = Kind::Generated;
    StageEnableFlags stage = SceneStage;
    int layer = 0;
    DrawingState state;
    std::string model, text;
    int baseIndex = 0, baseVertex = 0, faceCount = 0;
    float x = 0, y = 0, height = 0;
};
struct AudioSegments {
    std::int16_t *first = nullptr, *second = nullptr;
    AudioOffset firstCount = 0, secondCount = 0;
};
class DesktopError : public std::runtime_error {
public:
    DesktopError(const std::string &operation, const std::string &reason)
        : std::runtime_error("desktop " + operation + ": " + reason) {}
};

// Only application operations cross this boundary. No device or SDK objects escape it.
class Host {
public:
    class Window {
    public:
        explicit Window(Host &owner) : owner(owner) {}
        int GetGameWidth() const { return owner.frame().window.gameWidth; }
        int GetGameHeight() const { return owner.frame().window.gameHeight; }
        int GetScreenWidth() const { return owner.frame().window.screenWidth; }
        int GetScreenHeight() const { return owner.frame().window.screenHeight; }
        bool IsActive() const { return owner.frame().window.active; }
        WindowStyle GetWindowStyle() const { return owner.frame().window.fullscreen ? WindowStyle::Fullscreen : WindowStyle::Windowed; }
        void SetWindowStyle(WindowStyle style) { owner.setFullscreen(style == WindowStyle::Fullscreen); }
    private:
        Host &owner;
    };
    Host();
    virtual ~Host() = default;
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
    virtual void initialize(const engine_sim::ResourcePaths &paths, const engine_sim::CompiledScene &scene) = 0;
    virtual void shutdown() noexcept = 0;
    virtual void StartFrame() = 0;
    virtual void EndFrame() = 0;
    virtual const FrameState &frame() const = 0;
    virtual bool IsKeyDown(Key key) = 0;
    virtual bool ProcessKeyDown(Key key) = 0;
    virtual bool ProcessMouseButtonDown(Button button) = 0;
    virtual bool ProcessMouseButtonUp(Button button) = 0;
    virtual int GetMouseWheel() = 0; // Cumulative wheel position, matching the existing controls.
    virtual void GetOsMousePos(int *x, int *y) = 0; // Window-local logical coordinates, origin bottom left.
    virtual void setFullscreen(bool fullscreen) = 0;
    virtual void uploadGeometry(const Vertex *vertices, std::size_t vertexCount,
                                const std::uint16_t *indices, std::size_t indexCount) = 0;
    virtual float measureText(const std::string &text, float height) const = 0;
    virtual void openAudio() = 0; // Mono signed S16, 44100 Hz, one-second ring.
    virtual void setAudioPlaying(bool playing) = 0;
    virtual AudioOffset audioCursor() = 0;
    virtual AudioSegments lockAudio(AudioOffset offset, AudioOffset count) = 0;
    virtual void unlockAudio(const AudioSegments &segments) = 0;
    virtual void readScreen(std::uint8_t *rgb, std::size_t capacity) = 0;

    Window *GetGameWindow() { return &window; }
    bool IsOpen() const { return frame().window.open; }
    float GetFrameLength() const { return frame().seconds; }
    float GetAverageFramerate() const { return frame().averageFps; }
    int GetScreenWidth() const { return frame().window.gameWidth; }
    int GetScreenHeight() const { return frame().window.gameHeight; }
    void loadModels(const engine_sim::CompiledScene &scene);
    Model *GetModelAsset(const char *name);
    void DrawModel(StageEnableFlags stage, const Model *model, int layer = 0);
    void drawGenerated(StageEnableFlags stage, int baseIndex, int baseVertex, int faceCount, int layer);
    void drawText(const std::string &text, float x, float y, float height, const Vector &color);
    void setSceneViewport(const Viewport &viewport) { drawing.viewport = viewport; }
    void clearPackets() { packets.clear(); }
    const std::vector<DrawPacket> &drawPackets() const { return packets; }
    void validateGeometry(const Vertex *vertices, std::size_t vertexCount,
                          const std::uint16_t *indices, std::size_t indexCount) const;
    DrawingState drawing;
private:
    Window window;
    std::vector<Model> models;
    std::vector<DrawPacket> packets;
};

class TextRenderer {
public:
    void SetHost(Host *value) { host = value; }
    void SetColor(const Vector &value) { color = value; }
    float CalculateWidth(const std::string &text, float height) const;
    void RenderText(const std::string &text, float x, float y, float height);
private:
    Host *host = nullptr;
    Vector color{1,1,1,1};
};

#ifdef _WIN32
std::unique_ptr<Host> makeWindowsHost(void *instance);
#endif
}
#endif
