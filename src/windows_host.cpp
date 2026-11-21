#include "../include/desktop_host.h"
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
    explicit WindowsHost(void *instance) : instance(instance) {}
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
    }
    void shutdown() noexcept override {
        if (closed) return;
        closed=true;
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
    }
    void StartFrame() override { clearPackets(); checked(engine.StartFrame(),"start frame"); snapshot(); }
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
    }
    const desktop::FrameState &frame() const override { return current; }
    bool IsKeyDown(desktop::Key key) override { return engine.IsKeyDown(keyCode(key)); }
    bool ProcessKeyDown(desktop::Key key) override { return engine.ProcessKeyDown(keyCode(key)); }
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
        ysAudioParameters parameters;
        parameters.m_bitsPerSample=16; parameters.m_channelCount=1; parameters.m_sampleRate=44100;
        auto *audio=engine.GetAudioDevice();
        if (!audio || source) throw desktop::DesktopError("audio", "missing device or duplicate open");
        audioBuffer=audio->CreateBuffer(&parameters,44100);
        if (!audioBuffer) throw desktop::DesktopError("audio", "buffer creation failed");
        source=audio->CreateSource(audioBuffer);
        if (!source) throw desktop::DesktopError("audio", "source creation failed");
        checked(source->SetPan(0),"audio pan"); checked(source->SetVolume(1),"audio volume");
    }
    void setAudioPlaying(bool playing) override {
        if (!source) throw desktop::DesktopError("audio", "source is not open");
        checked(source->SetMode(playing?ysAudioSource::Mode::Loop:ysAudioSource::Mode::Stop),"audio mode");
    }
    void quiesceAudio() noexcept override {
        if (source) source->SetMode(ysAudioSource::Mode::Stop);
    }
    desktop::AudioOffset audioCursor() override {
        if (!source) throw desktop::DesktopError("audio", "source is not open");
        return source->GetCurrentWritePosition();
    }
    desktop::AudioSegments lockAudio(desktop::AudioOffset offset,desktop::AudioOffset count) override {
        if (!source || offset>=44100 || count>44100) throw desktop::DesktopError("audio", "invalid write span");
        void *first=nullptr,*second=nullptr; SampleOffset a=0,b=0;
        checked(source->LockBufferSegment(offset,count,&first,&a,&second,&b),"audio lock");
        return {static_cast<std::int16_t *>(first),static_cast<std::int16_t *>(second),a,b};
    }
    void unlockAudio(const desktop::AudioSegments &segments) override {
        checked(source->UnlockBufferSegments(segments.first,segments.firstCount,segments.second,segments.secondCount),"audio unlock");
    }
    void readScreen(std::uint8_t *rgba,std::size_t capacity) override {
        const auto required=static_cast<std::size_t>(engine.GetScreenWidth())*engine.GetScreenHeight()*4;
        if (!rgba || capacity<required) throw desktop::DesktopError("readback", "RGBA destination is too small");
        checked(engine.GetDevice()->ReadRenderTarget(engine.GetScreenRenderTarget(),rgba),"screen readback");
    }
private:
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

std::unique_ptr<desktop::Host> desktop::makeWindowsHost(void *instance) {
    return std::make_unique<WindowsHost>(instance);
}
