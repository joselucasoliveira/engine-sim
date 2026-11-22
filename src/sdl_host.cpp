#include "desktop_run.h"
#include "sdl_window.h"
#include "sdl_renderer.h"
#include "sdl_audio.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <atomic>

namespace desktop {
namespace {
class SdlHost final : public Host {
public:
    explicit SdlHost(RunOptions options) : options(std::move(options)) {}
    ~SdlHost() override { shutdown(); }
    void initialize(const engine_sim::ResourcePaths &paths, const engine_sim::CompiledScene &scene) override {
        window.initialize({});
        renderer.initialize(window.nativeHandle(), paths, scene);
        current = window.frame();
        if (runEvidence) { runEvidence->videoDriver = window.videoDriver(); runEvidence->renderer = renderer.statistics().driver; }
    }
    void shutdown() noexcept override {
        audio.shutdown(); renderer.shutdown(); window.shutdown();
        if (options.virtualAudio) SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
        if (runEvidence) runEvidence->tornDown = !audio.running() && window.nativeHandle()==nullptr;
    }
    void StartFrame() override {
        clearPackets();
        if (runEvidence) runEvidence->beforeFrame(*this);
        if (runEvidence && runEvidence->cylinders==9
            && (runEvidence->frames==1 || runEvidence->acceptedReloads!=framedReload)) {
            // Frame the supplied radial through its existing wheel control after
            // the first layout. No camera or geometry formula changes.
            SDL_Event motion{}; motion.type=SDL_EVENT_MOUSE_MOTION;
            motion.motion.windowID=SDL_GetWindowID(window.nativeHandle());
            motion.motion.x=drawing.viewport.x+drawing.viewport.width/2.0f;
            motion.motion.y=drawing.viewport.y+drawing.viewport.height/2.0f;
            SDL_Event wheel{}; wheel.type=SDL_EVENT_MOUSE_WHEEL; wheel.wheel.windowID=motion.motion.windowID;
            wheel.wheel.y=-8;
            if (!SDL_PushEvent(&motion) || !SDL_PushEvent(&wheel)) throw DesktopError("smoke radial framing",SDL_GetError());
            framedReload=runEvidence->acceptedReloads;
        }
        window.pumpEvents(); audio.check(); current = window.frame();
        if (runEvidence && runEvidence->frames >= options.frames) current.window.open = false;
    }
    void EndFrame() override {
        renderer.render(*this, current);
        if (runEvidence) {
            const auto stats = audio.statistics();
            runEvidence->audioConsumedInput = stats.consumedInput;
            runEvidence->audioPumpedSource = stats.pumpedSource;
            runEvidence->silenceFrames = stats.primingSilence + stats.starvationSilence;
            runEvidence->queueMaximum = stats.maxQueuedFrames;
            runEvidence->nonzeroFedToBackend = nonzeroFed.load();
            runEvidence->afterFrame(*this);
        }
    }
    const FrameState &frame() const override { return current; }
    bool IsKeyDown(Key key) override { const bool value = window.isKeyDown(key); return runEvidence ? runEvidence->key(key,value,false) : value; }
    bool ProcessKeyDown(Key key) override { const bool value = window.processKeyDown(key); return runEvidence ? runEvidence->key(key,value,true) : value; }
    bool ProcessMouseButtonDown(Button button) override { return window.processMouseDown(button); }
    bool ProcessMouseButtonUp(Button button) override { return window.processMouseUp(button); }
    int GetMouseWheel() override { return window.mouseWheel(); }
    void GetOsMousePos(int *x, int *y) override { window.mousePosition(x,y); }
    void setFullscreen(bool value) override { window.setFullscreen(value); current = window.frame(); }
    void uploadGeometry(const Vertex *v, std::size_t n, const std::uint16_t *i, std::size_t m) override {
        validateGeometry(v,n,i,m); renderer.uploadGeometry(v,n,i,m);
    }
    float measureText(const std::string &text, float height) const override { return renderer.measureText(text,height); }
    void openAudio() override {
        if (options.virtualAudio && !SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER,"dummy",SDL_HINT_OVERRIDE))
            throw DesktopError("audio", "could not select the explicit virtual driver");
        SdlAudioOutput::Options selected;
        selected.mode = options.virtualAudio ? SdlAudioOutput::Mode::Virtual : SdlAudioOutput::Mode::Device;
        if (runEvidence) {
            selected.observer=&nonzeroFed;
            selected.observeSource=[](void *context,const std::int16_t *samples,std::size_t count) noexcept {
                std::uint64_t nonzero=0;
                for (std::size_t i=0;i<count;++i) if (samples[i]) ++nonzero;
                static_cast<std::atomic<std::uint64_t> *>(context)->fetch_add(nonzero);
            };
        }
        audio.open(selected);
        if (runEvidence) {
            const auto d = audio.deviceInfo(); runEvidence->audioDriver = d.driver; runEvidence->audioDevice = d.name;
            runEvidence->deviceFrequency = d.deviceFrequency; runEvidence->deviceChannels = d.deviceChannels;
            runEvidence->deviceFormat = d.deviceFormat; runEvidence->deviceQuantum = d.deviceQuantum;
        }
    }
    void setAudioPlaying(bool value) override { audio.setPlaying(value); }
    void quiesceAudio() noexcept override { audio.quiesce(); }
    AudioOffset audioCursor() override { return audio.cursor(); }
    AudioProgress audioProgress() override { return audio.progress(); }
    void setAudioSourcePaused(bool paused) override { audio.setSourcePaused(paused); }
    void resetAudioQueue() override { audio.resetQueue(); }
    AudioSegments lockAudio(AudioOffset offset, AudioOffset count) override { return audio.lock(offset,count); }
    void unlockAudio(const AudioSegments &segments) override { audio.unlock(segments); if (runEvidence) runEvidence->source(segments); }
    void readScreen(std::uint8_t *rgba, std::size_t capacity) override {
        const auto pixels = renderer.readPixels();
        if (!rgba || capacity < pixels.size()) throw DesktopError("readback", "RGBA destination is too small");
        std::memcpy(rgba,pixels.data(),pixels.size());
    }
    void queueKeyEvent(Key key, bool down) override {
        SDL_Scancode scan;
        switch (key) {
        case Key::A: scan=SDL_SCANCODE_A; break;
        case Key::S: scan=SDL_SCANCODE_S; break;
        case Key::R: scan=SDL_SCANCODE_R; break;
        case Key::Tab: scan=SDL_SCANCODE_TAB; break;
        case Key::Return: scan=SDL_SCANCODE_RETURN; break;
        case Key::Right: scan=SDL_SCANCODE_RIGHT; break;
        default: throw DesktopError("smoke input", "unsupported scheduled key");
        }
        SDL_Event focus{}; focus.type=SDL_EVENT_WINDOW_FOCUS_GAINED; focus.window.windowID=SDL_GetWindowID(window.nativeHandle());
        if (!SDL_PushEvent(&focus)) throw DesktopError("smoke focus event",SDL_GetError());
        SDL_Event event{}; event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
        event.key.windowID=focus.window.windowID; event.key.scancode=scan; event.key.key=SDL_GetKeyFromScancode(scan,SDL_KMOD_NONE,false);
        event.key.down=down;
        if (!SDL_PushEvent(&event)) throw DesktopError("smoke key event",SDL_GetError());
    }
    void resizeForSmoke(int width,int height) override {
        const auto request = [&] {
            if (!SDL_RestoreWindow(window.nativeHandle())) throw DesktopError("smoke restore",SDL_GetError());
            if (!SDL_SyncWindow(window.nativeHandle())) throw DesktopError("smoke restore synchronization",SDL_GetError());
            if (!SDL_SetWindowSize(window.nativeHandle(),width,height)) throw DesktopError("smoke resize",SDL_GetError());
            return SDL_SyncWindow(window.nativeHandle());
        };
        // Some desktops report automatic maximization only while synchronizing
        // the first size request. Restore that observed state and retry once.
        if (!request() && (!(SDL_GetWindowFlags(window.nativeHandle()) & SDL_WINDOW_MAXIMIZED) || !request()))
            throw DesktopError("smoke resize synchronization",SDL_GetError());
    }
private:
    RunOptions options;
    SdlWindow window;
    SdlRenderer renderer;
    SdlAudioOutput audio;
    FrameState current;
    std::atomic<std::uint64_t> nonzeroFed{0};
    std::uint64_t framedReload=0;
};
}
std::unique_ptr<Host> makeSdlHost(const RunOptions &options) { return std::make_unique<SdlHost>(options); }
}
