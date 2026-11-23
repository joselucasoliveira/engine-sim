#ifndef ENGINE_SIM_TEST_RECORDING_HOST_H
#define ENGINE_SIM_TEST_RECORDING_HOST_H

#include "desktop_host.h"
#include <algorithm>
#include <functional>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace host_test {
struct Input {
    desktop::FrameState frame;
    std::array<bool,static_cast<std::size_t>(desktop::Key::Count)> held{}, pressed{};
    int wheel=0, mouseX=0, mouseY=0;
    bool mouseDown=false, mouseUp=false;
    Input() { frame.seconds=1.0f/60; }
    Input &hold(desktop::Key key) { held.at(static_cast<std::size_t>(key))=true; return *this; }
    Input &press(desktop::Key key) { hold(key); pressed.at(static_cast<std::size_t>(key))=true; return *this; }
};
struct RecordedFrame {
    desktop::FrameState frame;
    std::vector<desktop::DrawPacket> packets;
    std::size_t vertices=0, indices=0;
};
struct Recording {
    std::vector<RecordedFrame> frames;
    std::vector<bool> audioModes, fullscreen;
    std::vector<std::int16_t> pcm;
    int initialized=0, audioOpened=0, shutdowns=0;
    std::function<void()> onShutdown;
};

// Explicit contract fixture. It exercises the real application, without claiming a device or pixels.
class RecordingHost final : public desktop::Host {
public:
    explicit RecordingHost(std::shared_ptr<Recording> recording) : recording(std::move(recording)) {}
    ~RecordingHost() override { shutdown(); }
    std::vector<Input> inputs;
    std::function<void(std::size_t)> onFrame;
    bool failInitialize=false, failAudio=false;
    std::filesystem::path outputDirectory;
    desktop::FrameState state;
    void initialize(const engine_sim::ResourcePaths &paths,const engine_sim::CompiledScene &scene) override {
        ++recording->initialized;
        loadModels(scene);
        entry=paths.model;
        if (!outputDirectory.empty()) {
            std::filesystem::create_directories(outputDirectory);
            std::ofstream info(outputDirectory/"fixture.txt");
            info<<"Explicit record/replay fixture; no native pixels or device output\nresources="<<paths.root.u8string()
                <<"\nmodel="<<paths.model.u8string()<<"\nmodels="<<scene.meshes.size()<<'\n';
        }
        if (failInitialize) throw desktop::DesktopError("initialize","injected startup failure");
    }
    void shutdown() noexcept override {
        if (closed) return;
        closed=true; ++recording->shutdowns;
        if (recording->onShutdown) recording->onShutdown();
        if (!outputDirectory.empty()) {
            std::ofstream pcm(outputDirectory/"submitted.s16le",std::ios::binary);
            for (auto sample : recording->pcm) {
                const auto bits=static_cast<std::uint16_t>(sample);
                pcm.put(static_cast<char>(bits&255)); pcm.put(static_cast<char>(bits>>8));
            }
            std::ofstream result(outputDirectory/"result.txt");
            result<<"initialized="<<recording->initialized<<"\naudio-opened="<<recording->audioOpened
                  <<"\nshutdowns="<<recording->shutdowns<<"\nrecorded-frames="<<recording->frames.size()
                  <<"\nsubmitted-frames="<<recording->pcm.size()<<"\naudio-modes=";
            for (bool value : recording->audioModes) result<<value<<',';
            result<<"\nfullscreen="; for (bool value : recording->fullscreen) result<<value<<','; result<<'\n';
        }
    }
    void StartFrame() override {
        clearPackets();
        if (next>=inputs.size()) { state.window.open=false; return; }
        input=inputs[next]; originalInput=input; state=input.frame; state.window.fullscreen=fullscreen;
        state.monotonicNanoseconds=static_cast<std::uint64_t>(next+1)*16666667;
        if (playing) cursor=(cursor+735)%44100;
        if (onFrame) onFrame(next);
        if (!outputDirectory.empty()) {
            std::error_code error;
            std::filesystem::copy_file(entry,outputDirectory/("entry-"+std::to_string(next)+".mr"),
                std::filesystem::copy_options::overwrite_existing,error);
        }
        ++next;
    }
    void EndFrame() override {
        recording->frames.push_back({state,drawPackets(),vertices,indices});
        if (outputDirectory.empty()) return;
        std::ofstream stream(outputDirectory/"frames.jsonl",std::ios::app);
        stream<<std::setprecision(9)<<"{\"frame\":"<<next-1<<",\"logical\":["<<state.window.gameWidth<<','<<state.window.gameHeight
              <<"],\"framebuffer\":["<<state.window.framebufferWidth<<','<<state.window.framebufferHeight
              <<"],\"seconds\":"<<state.seconds<<",\"vertices\":"<<vertices<<",\"indices\":"<<indices<<",\"held\":[";
        for (std::size_t i=0;i<input.held.size();++i) { if(i)stream<<','; stream<<input.held[i]; }
        stream<<"],\"pressed\":[";
        for (std::size_t i=0;i<originalInput.pressed.size();++i) { if(i)stream<<','; stream<<originalInput.pressed[i]; }
        stream<<"],\"mouse_edges\":["<<originalInput.mouseDown<<','<<originalInput.mouseUp<<"],\"active\":"<<state.window.active
              <<",\"fullscreen\":"<<state.window.fullscreen<<",\"wheel\":"<<input.wheel
              <<",\"mouse\":["<<input.mouseX<<','<<input.mouseY<<"],\"draws\":[";
        bool first=true;
        for (const auto &packet : drawPackets()) {
            if (!first) stream<<',';
            first=false;
            stream<<"{\"kind\":"<<static_cast<int>(packet.kind)<<",\"stage\":"<<packet.stage<<",\"layer\":"<<packet.layer
                  <<",\"model\":"<<quote(packet.model)<<",\"text\":"<<quote(packet.text)
                  <<",\"range\":["<<packet.baseIndex<<','<<packet.baseVertex<<','<<packet.faceCount<<"],\"placement\":["
                  <<packet.x<<','<<packet.y<<','<<packet.height<<"],\"viewport\":["<<packet.state.viewport.x<<','<<packet.state.viewport.y
                  <<','<<packet.state.viewport.width<<','<<packet.state.viewport.height<<"],\"color\":";
            vector(stream,packet.state.color);
            stream<<",\"matrices\":["; bool firstMatrix=true;
            for (const auto *matrix : {&packet.state.transform,&packet.state.scene.view,&packet.state.scene.projection,
                                       &packet.state.ui.view,&packet.state.ui.projection}) {
                if (!firstMatrix) stream<<',';
                firstMatrix=false; stream<<'[';
                for (unsigned i=0;i<4;++i) { if(i)stream<<','; vector(stream,matrix->rows[i]); }
                stream<<']';
            }
            stream<<"]}";
        }
        stream<<"]}\n";
    }
    const desktop::FrameState &frame() const override { return state; }
    bool IsKeyDown(desktop::Key key) override { return input.held.at(static_cast<std::size_t>(key)); }
    bool ProcessKeyDown(desktop::Key key) override { return std::exchange(input.pressed.at(static_cast<std::size_t>(key)),false); }
    bool ProcessMouseButtonDown(desktop::Button button) override {
        return button==desktop::Button::Left && state.window.active && std::exchange(input.mouseDown,false);
    }
    bool ProcessMouseButtonUp(desktop::Button button) override {
        return button==desktop::Button::Left && state.window.active && std::exchange(input.mouseUp,false);
    }
    int GetMouseWheel() override { return input.wheel; }
    void GetOsMousePos(int *x,int *y) override { if (x) *x=input.mouseX; if (y) *y=input.mouseY; }
    void setFullscreen(bool full) override { fullscreen=full; state.window.fullscreen=full; recording->fullscreen.push_back(full); }
    void uploadGeometry(const desktop::Vertex *data,std::size_t count,const std::uint16_t *elements,std::size_t elementCount) override {
        validateGeometry(data,count,elements,elementCount); vertices=count; indices=elementCount;
        if (!outputDirectory.empty()) {
            const auto stem="geometry-"+std::to_string(next-1);
            std::ofstream v(outputDirectory/(stem+".vertices"),std::ios::binary);
            v.write(reinterpret_cast<const char *>(data),static_cast<std::streamsize>(count*sizeof(desktop::Vertex)));
            std::ofstream i(outputDirectory/(stem+".indices"),std::ios::binary);
            i.write(reinterpret_cast<const char *>(elements),static_cast<std::streamsize>(elementCount*sizeof(std::uint16_t)));
        }
    }
    float measureText(const std::string &text,float height) const override {
        // Generated monospace metrics are a contract fixture, never a real font qualification.
        std::size_t longest=0,line=0;
        for (char c : text) { if (c=='\n') { longest=std::max(longest,line); line=0; } else ++line; }
        return static_cast<float>(std::max(longest,line))*height*0.5f;
    }
    void openAudio() override {
        ++recording->audioOpened;
        if (failAudio) throw desktop::DesktopError("audio","injected open failure");
        opened=true;
    }
    void setAudioPlaying(bool value) override {
        if (!opened) throw desktop::DesktopError("audio","not open");
        playing=value; recording->audioModes.push_back(value);
    }
    void quiesceAudio() noexcept override { playing=false; }
    desktop::AudioOffset audioCursor() override { return cursor; }
    void resetAudioQueue() override { ring.fill(0); cursor=0; }
    desktop::AudioSegments lockAudio(desktop::AudioOffset offset,desktop::AudioOffset count) override {
        if (!opened || offset>=ring.size() || count>ring.size()) throw desktop::DesktopError("audio","invalid span");
        const auto first=std::min<desktop::AudioOffset>(count,static_cast<desktop::AudioOffset>(ring.size())-offset);
        return {ring.data()+offset,ring.data(),first,count-first};
    }
    void unlockAudio(const desktop::AudioSegments &segments) override {
        recording->pcm.insert(recording->pcm.end(),segments.first,segments.first+segments.firstCount);
        recording->pcm.insert(recording->pcm.end(),segments.second,segments.second+segments.secondCount);
    }
    void readScreen(std::uint8_t *,std::size_t) override {
        throw desktop::DesktopError("readback","the explicit recording fixture does not present pixels");
    }
private:
    static std::string quote(const std::string &value) {
        std::ostringstream stream; stream<<'"';
        for (unsigned char c : value) {
            if(c=='"' || c=='\\') stream<<'\\'<<c;
            else if(c<32) stream<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(c)<<std::dec;
            else stream<<c;
        }
        stream<<'"'; return stream.str();
    }
    static void vector(std::ostream &stream,const desktop::Vector &v) { stream<<'['<<v.x<<','<<v.y<<','<<v.z<<','<<v.w<<']'; }
    std::shared_ptr<Recording> recording;
    std::filesystem::path entry;
    Input input,originalInput;
    std::array<std::int16_t,44100> ring{};
    desktop::AudioOffset cursor=0;
    std::size_t next=0, vertices=0, indices=0;
    bool opened=false,playing=false,closed=false,fullscreen=false;
};
}
#endif
