#ifndef ENGINE_SIM_TEST_RENDERING_HOST_H
#define ENGINE_SIM_TEST_RENDERING_HOST_H
#include "sdl_renderer.h"
#include "sdl_window.h"
#include "recording_host.h"
#include <SDL3/SDL.h>
#include <cstring>
#include <set>

namespace render_test {
struct Output {
    desktop::SdlRenderer::Statistics gpu;
    std::set<std::string> models, labels;
    std::vector<desktop::Viewport> sceneViews;
    std::shared_ptr<host_test::Recording> audio=std::make_shared<host_test::Recording>();
    int shutdowns=0;
    std::function<void()> onShutdown;
};
inline void ppm(const std::filesystem::path &path,const std::vector<std::uint8_t> &pixels,int width,int height) {
    std::ofstream stream(path,std::ios::binary); stream<<"P6\n"<<width<<' '<<height<<"\n255\n";
    for (std::size_t i=0;i<pixels.size();i+=4) stream.write(reinterpret_cast<const char *>(pixels.data()+i),3);
    if (!stream) throw std::runtime_error("could not save GPU readback");
}
// Real video/GPU/input and application; the audio transport is explicitly a test ring.
class RenderingHost final : public desktop::Host {
public:
    explicit RenderingHost(std::shared_ptr<Output> output,std::filesystem::path directory,std::size_t frames)
        : output(std::move(output)),directory(std::move(directory)),audio(this->output->audio),frames(frames) { audio.inputs.resize(frames); }
    ~RenderingHost() override { shutdown(); }
    bool startEngine=false, tabs=false;
    std::function<void(std::size_t,SDL_Window *)> beforeEvents;
    void initialize(const engine_sim::ResourcePaths &paths,const engine_sim::CompiledScene &scene) override {
        window.initialize({"Engine Simulator application render integration",startEngine?2560:1920,startEngine?1440:1080,false});
        renderer.initialize(window.nativeHandle(),paths,scene); current=window.frame();
    }
    void shutdown() noexcept override {
        if (closed) return;
        closed=true; ++output->shutdowns;
        if (output->onShutdown) output->onShutdown();
        output->gpu=renderer.statistics(); audio.shutdown(); renderer.shutdown(); window.shutdown();
    }
    void StartFrame() override {
        clearPackets();
        if (next>=frames) { current.window.open=false; return; }
        SDL_Event focus{};focus.type=SDL_EVENT_WINDOW_FOCUS_GAINED;focus.window.windowID=SDL_GetWindowID(window.nativeHandle());
        if (!SDL_PushEvent(&focus)) throw desktop::DesktopError("test focus",SDL_GetError());
        if (startEngine && next==0) { key(SDL_SCANCODE_S,true);key(SDL_SCANCODE_A,true); }
        if (startEngine && next==1) key(SDL_SCANCODE_A,false);
        if (startEngine && next==1) {
            const auto &size=window.frame().window;
            SDL_Event motion{};motion.type=SDL_EVENT_MOUSE_MOTION;motion.motion.windowID=SDL_GetWindowID(window.nativeHandle());
            motion.motion.x=size.gameWidth/2.0f;motion.motion.y=size.gameHeight/4.0f;
            if (!SDL_PushEvent(&motion)) throw desktop::DesktopError("test mouse",SDL_GetError());
            SDL_Event wheel{};wheel.type=SDL_EVENT_MOUSE_WHEEL;wheel.wheel.windowID=motion.motion.windowID;wheel.wheel.y=-8;
            if (!SDL_PushEvent(&wheel)) throw desktop::DesktopError("test wheel",SDL_GetError());
        }
        if (tabs && (next==1 || next==3)) key(SDL_SCANCODE_TAB,true);
        if (tabs && next==2) key(SDL_SCANCODE_TAB,false);
        if (beforeEvents) beforeEvents(next,window.nativeHandle());
        window.pumpEvents(); current=window.frame();
        // A scheduled fixture clock keeps simulation assertions independent of GPU speed.
        current.seconds=1.0f/60;current.averageFps=60;audio.StartFrame();++next;
    }
    void EndFrame() override {
        renderer.render(*this,current);
        bool viewportRecorded=false;
        for (const auto &packet:drawPackets()) {
            if (!packet.model.empty()) output->models.insert(packet.model);
            if (!packet.text.empty()) output->labels.insert(packet.text);
            if (packet.stage==desktop::SceneStage && !viewportRecorded) {
                output->sceneViews.push_back(packet.state.viewport);viewportRecorded=true;
            }
        }
        if (next<=5 || next==frames) {
            const auto pixels=renderer.readPixels();
            ppm(directory/("frame-"+std::to_string(next)+".ppm"),pixels,current.window.framebufferWidth,current.window.framebufferHeight);
            const auto &s=renderer.statistics();
            std::ofstream info(directory/("frame-"+std::to_string(next)+".txt"));
            info<<"driver="<<s.driver<<"\nsubmitted="<<s.submittedFrames<<"\npresented="<<s.presentedFrames
                <<"\ndraws="<<s.draws<<"\nglyphs="<<s.glyphs<<"\nexplicit-test-audio-ring=true\n";
        }
    }
    const desktop::FrameState &frame() const override { return current; }
    bool IsKeyDown(desktop::Key key) override { return window.isKeyDown(key); }
    bool ProcessKeyDown(desktop::Key key) override { return window.processKeyDown(key); }
    bool ProcessMouseButtonDown(desktop::Button button) override { return window.processMouseDown(button); }
    bool ProcessMouseButtonUp(desktop::Button button) override { return window.processMouseUp(button); }
    int GetMouseWheel() override { return window.mouseWheel(); }
    void GetOsMousePos(int *x,int *y) override { window.mousePosition(x,y); }
    void setFullscreen(bool full) override { window.setFullscreen(full); }
    void uploadGeometry(const desktop::Vertex *v,std::size_t n,const std::uint16_t *i,std::size_t count) override {
        validateGeometry(v,n,i,count);renderer.uploadGeometry(v,n,i,count);
    }
    float measureText(const std::string &value,float height) const override { return renderer.measureText(value,height); }
    void openAudio() override { audio.openAudio(); }
    void setAudioPlaying(bool playing) override { audio.setAudioPlaying(playing); }
    void quiesceAudio() noexcept override { audio.quiesceAudio(); }
    desktop::AudioOffset audioCursor() override { return audio.audioCursor(); }
    desktop::AudioSegments lockAudio(desktop::AudioOffset offset,desktop::AudioOffset count) override { return audio.lockAudio(offset,count); }
    void unlockAudio(const desktop::AudioSegments &segments) override { audio.unlockAudio(segments); }
    void readScreen(std::uint8_t *target,std::size_t capacity) override {
        const auto pixels=renderer.readPixels();if (!target || capacity<pixels.size()) throw desktop::DesktopError("readback","destination is too small");
        std::memcpy(target,pixels.data(),pixels.size());
    }
private:
    void key(SDL_Scancode code,bool down) {
        SDL_Event e{};e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;e.key.windowID=SDL_GetWindowID(window.nativeHandle());
        e.key.scancode=code;e.key.down=down;if (!SDL_PushEvent(&e)) throw desktop::DesktopError("test key",SDL_GetError());
    }
    std::shared_ptr<Output> output;
    std::filesystem::path directory;
    host_test::RecordingHost audio;
    desktop::SdlWindow window;
    desktop::SdlRenderer renderer;
    desktop::FrameState current;
    std::size_t frames,next=0;
    bool closed=false;
};
}
#endif
