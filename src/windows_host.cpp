#include "../include/desktop_host.h"
#include "desktop_run.h"
#include "virtual_audio.h"
#include <delta-studio/include/yds_windows_window.h>
#include <delta-studio/include/yds_core.h>
#include <delta-studio/engines/basic/include/delta_basic_engine.h>
#include <chrono>
#include <limits>

namespace {
// The existing Windows shader stages stay private to the Delta adapter.
class WindowsShaders : public dbasic::ShaderBase {
    public:
        WindowsShaders();
        ~WindowsShaders();

        ysError Initialize(
                dbasic::ShaderSet *shaderSet,
                ysRenderTarget *mainRenderTarget,
                ysRenderTarget *uiRenderTarget,
                ysShaderProgram *shaderProgram,
                ysInputLayout *inputLayout);
        virtual ysError UseMaterial(dbasic::Material *material);
		virtual void SetObjectTransform(const ysMatrix &mat);
		virtual void ConfigureModel(float scale, dbasic::ModelAsset *model);

        void SetBaseColor(const ysVector &color);
        void ResetBaseColor();

        dbasic::StageEnableFlags GetRegularFlags() const;
        dbasic::StageEnableFlags GetUiFlags() const;

        void SetClearColor(const ysVector &col);

    public:
        dbasic::ShaderScreenVariables m_screenVariables;
        dbasic::ShaderScreenVariables m_uiScreenVariables;
        dbasic::ShaderObjectVariables m_objectVariables;

        ysVector m_cameraPosition;

    protected:
        dbasic::ShaderStage *m_mainStage;
        dbasic::ShaderStage *m_uiStage;

        dbasic::LightingControls m_lightingControls;
};


WindowsShaders::WindowsShaders() {
    m_cameraPosition = ysMath::LoadVector(0.0f, 0.0f);

    m_mainStage = nullptr;
    m_uiStage = nullptr;

    m_objectVariables.ColorReplace = 1;
    m_objectVariables.Lit = 0;
    m_objectVariables.Transform = ysMath::LoadIdentity();

    m_screenVariables.FogNear = m_uiScreenVariables.FogNear = 16000.0f;
    m_screenVariables.FogFar = m_uiScreenVariables.FogFar = 16001.0f;
}

WindowsShaders::~WindowsShaders() {
    /* void */
}

ysError WindowsShaders::Initialize(
        dbasic::ShaderSet *shaderSet,
        ysRenderTarget *mainRenderTarget,
        ysRenderTarget *uiRenderTarget,
        ysShaderProgram *shaderProgram,
        ysInputLayout *inputLayout)
{
    YDS_ERROR_DECLARE("Initialize");

    YDS_NESTED_ERROR_CALL(shaderSet->NewStage("ShaderStage::Main", &m_mainStage));
    YDS_NESTED_ERROR_CALL(shaderSet->NewStage("ShaderStage::UI", &m_uiStage));

    m_mainStage->SetInputLayout(inputLayout);
    m_mainStage->SetRenderTarget(mainRenderTarget);
    m_mainStage->SetShaderProgram(shaderProgram);
    m_mainStage->SetFlagBit(0);
    m_mainStage->SetType(dbasic::ShaderStage::Type::FullPass);

    m_mainStage->NewConstantBuffer<dbasic::ShaderScreenVariables>(
        "Buffer::ScreenData",
        0,
        dbasic::ShaderStage::ConstantBufferBinding::BufferType::SceneData,
        &m_screenVariables);
    m_mainStage->NewConstantBuffer<dbasic::ShaderObjectVariables>(
        "Buffer::ObjectData",
        1,
        dbasic::ShaderStage::ConstantBufferBinding::BufferType::ObjectData,
        &m_objectVariables);
    m_mainStage->NewConstantBuffer<dbasic::LightingControls>(
        "Buffer::LightingData",
        3,
        dbasic::ShaderStage::ConstantBufferBinding::BufferType::SceneData,
        &m_lightingControls);

    // UI Stage
    m_uiStage->SetInputLayout(inputLayout);
    m_uiStage->SetRenderTarget(uiRenderTarget);
    m_uiStage->SetShaderProgram(shaderProgram);
    m_uiStage->SetFlagBit(1);
    m_uiStage->SetClearTarget(false);
    m_uiStage->SetType(dbasic::ShaderStage::Type::FullPass);

    m_uiStage->NewConstantBuffer<dbasic::ShaderScreenVariables>(
        "Buffer::ScreenData",
        0,
        dbasic::ShaderStage::ConstantBufferBinding::BufferType::SceneData,
        &m_uiScreenVariables);
    m_uiStage->NewConstantBuffer<dbasic::ShaderObjectVariables>(
        "Buffer::ObjectData",
        1,
        dbasic::ShaderStage::ConstantBufferBinding::BufferType::ObjectData,
        &m_objectVariables);
    m_uiStage->NewConstantBuffer<dbasic::LightingControls>(
        "Buffer::LightingData",
        3,
        dbasic::ShaderStage::ConstantBufferBinding::BufferType::SceneData,
        &m_lightingControls);

    return YDS_ERROR_RETURN(ysError::None);
}

ysError WindowsShaders::UseMaterial(dbasic::Material *material) {
    YDS_ERROR_DECLARE("UseMaterial");
    return YDS_ERROR_RETURN(ysError::None);
}

void WindowsShaders::SetObjectTransform(const ysMatrix &mat) {
    m_objectVariables.Transform = mat;
}

void WindowsShaders::ConfigureModel(float scale, dbasic::ModelAsset *model) {
    /* void */
}

void WindowsShaders::SetBaseColor(const ysVector &color) {
    m_objectVariables.BaseColor = color;
}

void WindowsShaders::ResetBaseColor() {
    m_objectVariables.BaseColor = ysMath::LoadVector(1.0f, 1.0f, 1.0f, 1.0f);
}

dbasic::StageEnableFlags WindowsShaders::GetRegularFlags() const {
    return m_mainStage->GetFlags();
}

dbasic::StageEnableFlags WindowsShaders::GetUiFlags() const {
    return m_uiStage->GetFlags();
}

void WindowsShaders::SetClearColor(const ysVector &col) {
    m_mainStage->SetClearColor(col);
}

}

namespace {
void checked(ysError result, const char *operation) {
    if (result != ysError::None) throw desktop::DesktopError(operation, "Delta error " + std::to_string(static_cast<int>(result)));
}
ysVector native(const desktop::Vector &v) { return ysMath::LoadVector(v.x,v.y,v.z,v.w); }
ysMatrix native(const desktop::Matrix &m) { return {{native(m.rows[0]),native(m.rows[1]),native(m.rows[2]),native(m.rows[3])}}; }

// Delta's successful shutdown order is retained, with null checks for failed startup.
class OwnedDeltaEngine : public dbasic::DeltaEngine {
public:
    using dbasic::DeltaEngine::ClearDrawQueue;
    OwnedDeltaEngine() {
        m_vertexSkinnedShader=nullptr; m_saqVertexShader=nullptr; m_saqPixelShader=nullptr;
        m_consoleShaderObjectVariablesBuffer=nullptr; m_timingSystem=nullptr;
    }
    ~OwnedDeltaEngine() { release(); }
    void release() noexcept {
        if (released) return;
        released=true;
        ClearDrawQueue();
        if (m_device) {
            auto *font=m_console.GetFont();
            if (font) { m_console.Destroy(); delete font; }
            if (m_uiRenderer.GetEngine()) m_uiRenderer.Destroy();
            if (m_mainIndexBuffer) m_device->DestroyGPUBuffer(m_mainIndexBuffer);
            if (m_mainVertexBuffer) m_device->DestroyGPUBuffer(m_mainVertexBuffer);
            if (m_consoleShaderObjectVariablesBuffer) m_device->DestroyGPUBuffer(m_consoleShaderObjectVariablesBuffer);
            for (auto **shader : {&m_vertexShader,&m_vertexSkinnedShader,&m_consoleVertexShader,&m_saqVertexShader,
                                  &m_pixelShader,&m_saqPixelShader,&m_consolePixelShader}) {
                if (*shader) m_device->DestroyShader(*shader);
            }
            for (auto **program : {&m_shaderProgram,&m_consoleProgram,&m_skinnedShaderProgram}) {
                if (*program) m_device->DestroyShaderProgram(*program);
            }
            for (auto **layout : {&m_inputLayout,&m_skinnedInputLayout,&m_consoleInputLayout,&m_saqInputLayout}) {
                if (*layout) m_device->DestroyInputLayout(*layout);
            }
            if (m_mainRenderTarget) m_device->DestroyRenderTarget(m_mainRenderTarget);
            if (m_renderingContext) m_device->DestroyRenderingContext(m_renderingContext);
            m_device->DestroyDevice();
            // The public context destructor dispatches to the SDK's protected device destructor.
            delete static_cast<ysContextObject *>(m_device); m_device=nullptr;
        }
        if (m_audioSystem) {
            if (m_audioDevice) m_audioSystem->DisconnectDevice(m_audioDevice);
            ysAudioSystem::DestroyAudioSystem(&m_audioSystem);
        }
        m_audioDevice=nullptr; m_mainKeyboard=nullptr; m_mainMouse=nullptr;
        if (m_inputSystem) ysInputSystem::DestroyInputSystem(m_inputSystem);
        if (m_windowSystem) {
            m_windowSystem->DeleteAllWindows(); m_gameWindow=nullptr;
            ysWindowSystem::DestroyWindowSystem(m_windowSystem);
        }
        m_initialized=false; m_shaderSet=nullptr;
    }
private:
    bool released=false;
};

ysKey::Code keyCode(desktop::Key key) {
    static constexpr ysKey::Code codes[]{
        ysKey::Code::Escape,ysKey::Code::Return,ysKey::Code::Tab,ysKey::Code::F,ysKey::Code::Insert,
        ysKey::Code::Right,ysKey::Code::Space,ysKey::Code::Shift,ysKey::Code::Up,ysKey::Code::Down,
        ysKey::Code::N1,ysKey::Code::N2,ysKey::Code::N3,ysKey::Code::N4,ysKey::Code::N5,
        ysKey::Code::F1,ysKey::Code::F2,ysKey::Code::F3,ysKey::Code::Z,ysKey::Code::X,ysKey::Code::C,
        ysKey::Code::V,ysKey::Code::B,ysKey::Code::N,ysKey::Code::G,ysKey::Code::Q,ysKey::Code::W,
        ysKey::Code::E,ysKey::Code::R,ysKey::Code::M,ysKey::Code::OEM_Comma,ysKey::Code::D,
        ysKey::Code::H,ysKey::Code::S,ysKey::Code::A,ysKey::Code::T,ysKey::Code::U,ysKey::Code::Y,ysKey::Code::I};
    static_assert(sizeof(codes)/sizeof(codes[0]) == static_cast<std::size_t>(desktop::Key::Count));
    const auto index=static_cast<std::size_t>(key);
    if (index>=static_cast<std::size_t>(desktop::Key::Count)) throw desktop::DesktopError("input", "invalid key");
    return codes[index];
}

class WindowsHost final : public desktop::Host {
public:
    explicit WindowsHost(void *instance,bool virtualAudio) : virtualMode(virtualAudio), instance(instance) {}
    ~WindowsHost() override { shutdown(); }
    void initialize(const engine_sim::ResourcePaths &paths, const engine_sim::CompiledScene &) override {
        if (started || closed) throw desktop::DesktopError("initialize", "host cannot be reopened");
        paths.validateLegacyDesktop();
        engine.GetConsole()->SetDefaultFontDirectory(paths.desktop.u8string()+"/fonts/");
        const auto shaders=paths.desktop.u8string()+"/shaders/";
        dbasic::DeltaEngine::GameEngineSettings settings;
        settings.API=ysContextObject::DeviceAPI::DirectX11; settings.DepthBuffer=false;
        settings.Instance=instance; settings.ShaderDirectory=shaders.c_str();
        settings.WindowTitle="Engine Simulator (independent fork)";
        settings.WindowPositionX=0; settings.WindowPositionY=0;
        settings.WindowWidth=1920; settings.WindowHeight=1080;
        settings.WindowStyle=ysWindow::WindowStyle::Windowed;
        checked(engine.CreateGameWindow(settings),"create window"); started=true;
        if (!engine.GetAudioDevice() || !engine.GetDevice() || !engine.GetConsole()->GetFont()) {
            throw desktop::DesktopError("initialize", "required graphics, font or audio device is unavailable");
        }
        checked(engine.GetDevice()->CreateSubRenderTarget(&mainTarget,engine.GetScreenRenderTarget(),0,0,0,0),"scene target");
        checked(engine.InitializeShaderSet(&shaderSet),"shader set"); shaderReady=true;
        checked(shader.Initialize(&shaderSet,mainTarget,engine.GetScreenRenderTarget(),
            engine.GetDefaultShaderProgram(),engine.GetDefaultInputLayout()),"shader stages");
        checked(engine.InitializeConsoleShaders(&shaderSet),"console shaders"); engine.SetShaderSet(&shaderSet);
        assets.SetEngine(&engine); assetsReady=true;
        checked(assets.LoadSceneFile((paths.assets.u8string()+"/assets").c_str(),true),"scene load");
        static_assert(sizeof(desktop::Vertex)==sizeof(dbasic::Vertex));
        checked(engine.GetDevice()->CreateIndexBuffer(&indexBuffer,sizeof(std::uint16_t)*200000,nullptr),"index buffer");
        checked(engine.GetDevice()->CreateVertexBuffer(&vertexBuffer,sizeof(desktop::Vertex)*100000,nullptr),"vertex buffer");
        text.SetEngine(&engine); text.SetRenderer(engine.GetUiRenderer()); text.SetFont(engine.GetConsole()->GetFont());
        snapshot();
        if (runEvidence) {
            runEvidence->videoDriver="win32"; runEvidence->renderer="Direct3D 11";
            runEvidence->audioDriver=virtualMode?"virtual-clocked-sink":"DirectSound";
            runEvidence->audioDevice=virtualMode?"Explicit clocked software sink":"Default DirectSound endpoint; native mix format is not exposed by Delta";
        }
    }
    void shutdown() noexcept override {
        if (closed) return;
        closed=true;
        for (std::size_t key=0; key<injectedKeys.size(); ++key) if (injectedKeys[key]) {
            INPUT input{}; input.type=INPUT_KEYBOARD;
            input.ki.wScan=static_cast<WORD>(MapVirtualKeyW(injectedKeys[key],MAPVK_VK_TO_VSC));
            input.ki.dwFlags=KEYEVENTF_SCANCODE|KEYEVENTF_KEYUP|(key==static_cast<std::size_t>(desktop::Key::Right)?KEYEVENTF_EXTENDEDKEY:0);
            SendInput(1,&input,sizeof(input)); injectedKeys[key]=0;
        }
        virtualSink.shutdown();
        clearPackets(); engine.ClearDrawQueue();
        if (auto *audio=engine.GetAudioDevice()) {
            if (source) { source->SetMode(ysAudioSource::Mode::Stop); audio->DestroyAudioSource(source); }
            if (audioBuffer) audio->DestroyAudioBuffer(audioBuffer);
        }
        if (shaderReady) { shaderSet.Destroy(); shaderReady=false; }
        if (auto *device=engine.GetDevice()) {
            if (vertexBuffer) device->DestroyGPUBuffer(vertexBuffer);
            if (indexBuffer) device->DestroyGPUBuffer(indexBuffer);
        }
        if (assetsReady) { assets.Destroy(); assetsReady=false; }
        engine.release(); started=false;
        if (runEvidence) runEvidence->tornDown=true;
    }
    void StartFrame() override {
        clearPackets(); if (runEvidence) runEvidence->beforeFrame(*this);
        checked(engine.StartFrame(),"start frame"); snapshot();
        if (runEvidence && runEvidence->frames>=runEvidence->options.frames) current.window.open=false;
    }
    void EndFrame() override {
        const auto &view=drawing.viewport;
        checked(engine.GetDevice()->ResizeRenderTarget(mainTarget,view.width,view.height,view.width,view.height),"resize scene");
        checked(engine.GetDevice()->RepositionRenderTarget(mainTarget,view.x,view.y),"position scene");
        shader.SetClearColor(native(drawing.clearColor));
        for (const auto &packet : drawPackets()) {
            apply(packet.state);
            if (packet.kind==desktop::DrawPacket::Kind::Text) {
                text.SetColor(native(packet.state.color)); text.RenderText(packet.text,packet.x,packet.y,packet.height);
            }
            else if (packet.kind==desktop::DrawPacket::Kind::Model) {
                auto *model=assets.GetModelAsset(packet.model.c_str());
                if (!model) throw desktop::DesktopError("model", "missing "+packet.model);
                checked(engine.DrawModel(packet.stage,model,packet.layer),"draw model");
            }
            else checked(engine.DrawGeneric(packet.stage,indexBuffer,vertexBuffer,sizeof(desktop::Vertex),
                packet.baseIndex,packet.baseVertex,packet.faceCount,false,packet.layer),"draw geometry");
        }
        checked(engine.EndFrame(),"present frame");
        if (runEvidence) {
            audioCursor();
            if (virtualMode) {
                runEvidence->audioConsumedInput=virtualSink.consumed;
                runEvidence->audioPumpedSource=virtualSink.source;
                runEvidence->silenceFrames=virtualSink.silence;
                runEvidence->nonzeroFedToBackend=virtualSink.nonzero;
            }
            runEvidence->afterFrame(*this);
        }
    }
    const desktop::FrameState &frame() const override { return current; }
    bool IsKeyDown(desktop::Key key) override { const bool value=engine.IsKeyDown(keyCode(key)); return runEvidence?runEvidence->key(key,value,false):value; }
    bool ProcessKeyDown(desktop::Key key) override { const bool value=engine.ProcessKeyDown(keyCode(key)); return runEvidence?runEvidence->key(key,value,true):value; }
    bool ProcessMouseButtonDown(desktop::Button button) override {
        return button==desktop::Button::Left && engine.ProcessMouseButtonDown(ysMouse::Button::Left);
    }
    bool ProcessMouseButtonUp(desktop::Button button) override {
        return button==desktop::Button::Left && engine.ProcessMouseButtonUp(ysMouse::Button::Left);
    }
    int GetMouseWheel() override { return engine.GetMouseWheel(); }
    void GetOsMousePos(int *x,int *y) override { engine.GetOsMousePos(x,y); }
    void setFullscreen(bool full) override {
        engine.GetGameWindow()->SetWindowStyle(full?ysWindow::WindowStyle::Fullscreen:ysWindow::WindowStyle::Windowed);
        snapshot();
    }
    void uploadGeometry(const desktop::Vertex *vertices,std::size_t vertexCount,
                        const std::uint16_t *indices,std::size_t indexCount) override {
        validateGeometry(vertices,vertexCount,indices,indexCount);
        checked(engine.GetDevice()->EditBufferDataRange(vertexBuffer,const_cast<char *>(reinterpret_cast<const char *>(vertices)),
            static_cast<int>(sizeof(desktop::Vertex)*vertexCount),0),"upload vertices");
        checked(engine.GetDevice()->EditBufferDataRange(indexBuffer,const_cast<char *>(reinterpret_cast<const char *>(indices)),
            static_cast<int>(sizeof(std::uint16_t)*indexCount),0),"upload indices");
    }
    float measureText(const std::string &value,float height) const override { return text.CalculateWidth(value,height); }
    void openAudio() override {
        if (virtualMode) { virtualSink.open(); return; }
        ysAudioParameters parameters;
        parameters.m_bitsPerSample=16; parameters.m_channelCount=1; parameters.m_sampleRate=44100;
        auto *audio=engine.GetAudioDevice();
        if (!audio || source) throw desktop::DesktopError("audio", "missing device or duplicate open");
        audioBuffer=audio->CreateBuffer(&parameters,44100);
        if (!audioBuffer) throw desktop::DesktopError("audio", "buffer creation failed");
        source=audio->CreateSource(audioBuffer);
        if (!source) throw desktop::DesktopError("audio", "source creation failed");
        checked(source->SetPan(0),"audio pan"); checked(source->SetVolume(1),"audio volume");
        resetAudioQueue();
    }
    void setAudioPlaying(bool playing) override {
        if (virtualMode) { virtualSink.setPlaying(playing); return; }
        if (!source) throw desktop::DesktopError("audio", "source is not open");
        checked(source->SetMode(playing?ysAudioSource::Mode::Loop:ysAudioSource::Mode::Stop),"audio mode");
        ringPlaying=playing; ringPoll=std::chrono::steady_clock::now();
    }
    void quiesceAudio() noexcept override {
        virtualSink.quiesce(); ringPlaying=false;
        if (source) source->SetMode(ysAudioSource::Mode::Stop);
    }
    void setAudioSourcePaused(bool value) override { ringPaused=value; }
    void resetAudioQueue() override {
        if (virtualMode) { virtualSink.shutdown(); virtualSink.open(); return; }
        if (!source) throw desktop::DesktopError("audio reset", "source is not open");
        checked(source->SetMode(ysAudioSource::Mode::Stop),"audio reset stop");
        void *data=nullptr; SampleOffset count=0;
        checked(source->LockEntireBuffer(&data,&count),"audio reset lock");
        std::fill_n(static_cast<std::int16_t *>(data),count,0);
        checked(source->UnlockBuffer(data,count),"audio reset unlock");
        source->Seek(0);
        ringTags.fill(0); std::fill_n(ringTags.begin(),4410,2);
        ringPosition=0; ringConsumed=0; ringWritten=4410;
        ringSubmitted=ringConsumedSource=ringStarvation=ringPauseSilence=0;
        ringPriming=4410; ++ringEpoch; ringPlaying=false; ringPaused=false;
    }
    desktop::AudioOffset audioCursor() override {
        if (virtualMode) return virtualSink.cursor();
        return audioProgress().cursor;
    }
    desktop::AudioProgress audioProgress() override {
        if (virtualMode) { desktop::AudioProgress value; value.cursor=virtualSink.cursor(); return value; }
        if (!source) throw desktop::DesktopError("audio", "source is not open");
        const auto now=std::chrono::steady_clock::now();
        if (ringPlaying && now-ringPoll>=std::chrono::seconds(1))
            throw desktop::DesktopError("audio progress", "one-second cursor deadline exceeded");
        ringPoll=now;
        const auto position=source->GetCurrentPosition();
        const auto advanced=(position+44100-ringPosition)%44100;
        for (unsigned i=0;i<advanced;++i) {
            const auto tag=ringTags[(ringPosition+i)%44100];
            if (tag==1) ++ringConsumedSource;
            else if (tag==0) { if (ringPaused) ++ringPauseSilence; else ++ringStarvation; }
            ringTags[(ringPosition+i)%44100]=0;
        }
        if (advanced) {
            // Retire every played cell before the looping native ring can replay it.
            auto span=lockAudio(ringPosition,advanced);
            std::fill_n(span.first,span.firstCount,0); std::fill_n(span.second,span.secondCount,0);
            checked(source->UnlockBufferSegments(span.first,span.firstCount,span.second,span.secondCount),"retire audio cells");
        }
        ringPosition=position; ringConsumed+=advanced;
        if (ringConsumed>ringWritten) {
            if (!ringPaused) throw desktop::DesktopError("audio progress", "device exhausted the bounded source lead");
            ringWritten=ringConsumed;
        }
        if (ringPaused && ringWritten-ringConsumed<4410) {
            const auto count=4410-(ringWritten-ringConsumed);
            for (std::uint64_t i=0;i<count;++i) ringTags[(ringWritten+i)%44100]=3;
            ringWritten+=count; ringPauseSilence+=count;
        }
        desktop::AudioProgress value;
        value.queueKnown=value.writeOffsetKnown=true;
        value.cursor=position; value.queued=static_cast<desktop::AudioOffset>(ringWritten-ringConsumed);
        value.writeOffset=static_cast<desktop::AudioOffset>(ringWritten%44100);
        const auto safe=(source->GetCurrentWritePosition()+44100-position)%44100;
        value.writable=value.queued>=safe?44100-value.queued:0;
        value.consumed=ringConsumed; value.submittedSource=ringSubmitted;
        value.consumedSource=ringConsumedSource; value.starvationSilence=ringStarvation;
        value.pauseSilence=ringPauseSilence; value.primingSilence=ringPriming; value.epoch=ringEpoch;
        if (runEvidence) runEvidence->audioConsumedInput=ringConsumed;
        return value;
    }
    desktop::AudioSegments lockAudio(desktop::AudioOffset offset,desktop::AudioOffset count) override {
        if (virtualMode) return virtualSink.lock(offset,count);
        if (!source || offset>=44100 || count>44100) throw desktop::DesktopError("audio", "invalid write span");
        void *first=nullptr,*second=nullptr; SampleOffset a=0,b=0;
        checked(source->LockBufferSegment(offset,count,&first,&a,&second,&b),"audio lock");
        return {static_cast<std::int16_t *>(first),static_cast<std::int16_t *>(second),a,b};
    }
    void unlockAudio(const desktop::AudioSegments &segments) override {
        if (virtualMode) { virtualSink.unlock(segments); if (runEvidence) runEvidence->source(segments); return; }
        if (runEvidence) {
            runEvidence->source(segments); runEvidence->audioPumpedSource=runEvidence->sourceFrames;
            runEvidence->nonzeroFedToBackend=runEvidence->nonzeroSource;
        }
        checked(source->UnlockBufferSegments(segments.first,segments.firstCount,segments.second,segments.secondCount),"audio unlock");
        const auto count=static_cast<std::uint64_t>(segments.firstCount)+segments.secondCount;
        for (std::uint64_t i=0;i<count;++i) ringTags[(ringWritten+i)%44100]=1;
        ringWritten+=count; ringSubmitted+=count;
    }
    void readScreen(std::uint8_t *rgba,std::size_t capacity) override {
        const auto required=static_cast<std::size_t>(engine.GetScreenWidth())*engine.GetScreenHeight()*4;
        if (!rgba || capacity<required) throw desktop::DesktopError("readback", "RGBA destination is too small");
        checked(engine.GetDevice()->ReadRenderTarget(engine.GetScreenRenderTarget(),rgba),"screen readback");
    }
    void queueKeyEvent(desktop::Key key,bool down) override {
        auto *window=static_cast<ysWindowsWindow *>(engine.GetGameWindow());
        const HWND hwnd=window->GetWindowHandle();
        if (GetForegroundWindow()!=hwnd && !SetForegroundWindow(hwnd)) throw desktop::DesktopError("smoke input","cannot focus the native window");
        WORD vk=0;
        switch (key) {
        case desktop::Key::A: vk='A'; break; case desktop::Key::S: vk='S'; break; case desktop::Key::R: vk='R'; break;
        case desktop::Key::Tab: vk=VK_TAB; break; case desktop::Key::Return: vk=VK_RETURN; break; case desktop::Key::Right: vk=VK_RIGHT; break;
        default: throw desktop::DesktopError("smoke input","unsupported scheduled key");
        }
        INPUT input{}; input.type=INPUT_KEYBOARD; input.ki.wScan=static_cast<WORD>(MapVirtualKeyW(vk,MAPVK_VK_TO_VSC));
        input.ki.dwFlags=KEYEVENTF_SCANCODE|(down?0:KEYEVENTF_KEYUP)|(key==desktop::Key::Right?KEYEVENTF_EXTENDEDKEY:0);
        if (SendInput(1,&input,sizeof(input))!=1) throw desktop::DesktopError("smoke input","SendInput failed");
        injectedKeys[static_cast<std::size_t>(key)]=down?vk:0;
    }
    void resizeForSmoke(int width,int height) override {
        const HWND hwnd=static_cast<ysWindowsWindow *>(engine.GetGameWindow())->GetWindowHandle();
        RECT bounds{0,0,width,height};
        if (!AdjustWindowRectEx(&bounds,static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(hwnd,GWL_EXSTYLE)))
            || !SetWindowPos(hwnd,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE))
            throw desktop::DesktopError("smoke resize","native window resize failed");
    }
private:
    bool virtualMode=false;
    desktop::VirtualAudio virtualSink;
    desktop::AudioOffset lastCursor=0;
    bool haveCursor=false;
    std::array<std::uint8_t,44100> ringTags{};
    desktop::AudioOffset ringPosition=0;
    std::uint64_t ringConsumed=0,ringWritten=0,ringSubmitted=0,ringConsumedSource=0;
    std::uint64_t ringStarvation=0,ringPauseSilence=0,ringPriming=0,ringEpoch=0;
    bool ringPlaying=false,ringPaused=false;
    std::chrono::steady_clock::time_point ringPoll;
    std::array<WORD,static_cast<std::size_t>(desktop::Key::Count)> injectedKeys{};
    void snapshot() {
        auto *window=engine.GetGameWindow();
        if (!window) { current.window.open=false; return; }
        current.window={window->GetGameWidth(),window->GetGameHeight(),window->GetScreenWidth(),window->GetScreenHeight(),
            window->GetGameWidth(),window->GetGameHeight(),window->IsActive(),engine.IsOpen(),
            window->GetWindowStyle()==ysWindow::WindowStyle::Fullscreen};
        current.seconds=engine.GetFrameLength(); current.averageFps=engine.GetAverageFramerate();
        current.monotonicNanoseconds=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void apply(const desktop::DrawingState &state) {
        shader.SetObjectTransform(native(state.transform)); shader.SetBaseColor(native(state.color));
        shader.m_screenVariables.CameraView=native(state.scene.view);
        shader.m_screenVariables.Projection=native(state.scene.projection);
        shader.m_screenVariables.Eye=ysMath::GetVector4(native(state.scene.eye));
        shader.m_uiScreenVariables.CameraView=native(state.ui.view);
        shader.m_uiScreenVariables.Projection=native(state.ui.projection);
        shader.m_uiScreenVariables.Eye=ysMath::GetVector4(native(state.ui.eye));
    }
    void *instance;
    OwnedDeltaEngine engine;
    desktop::FrameState current;
    dbasic::ShaderSet shaderSet;
    WindowsShaders shader;
    dbasic::AssetManager assets;
    dbasic::TextRenderer text;
    ysRenderTarget *mainTarget=nullptr;
    ysGPUBuffer *vertexBuffer=nullptr,*indexBuffer=nullptr;
    ysAudioBuffer *audioBuffer=nullptr;
    ysAudioSource *source=nullptr;
    bool started=false,closed=false,shaderReady=false,assetsReady=false;
};
}

std::unique_ptr<desktop::Host> desktop::makeWindowsHost(void *instance,bool virtualAudio) {
    return std::make_unique<WindowsHost>(instance,virtualAudio);
}
