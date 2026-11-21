#include "../include/engine_sim_application.h"

#include "../include/piston_object.h"
#include "../include/connecting_rod_object.h"
#include "../include/constants.h"
#include "../include/units.h"
#include "../include/crankshaft_object.h"
#include "../include/cylinder_bank_object.h"
#include "../include/cylinder_head_object.h"
#include "../include/ui_button.h"
#include "../include/combustion_chamber_object.h"
#include "../include/exhaust_system.h"
#include "../include/feedback_comb_filter.h"
#include "../include/utilities.h"
#include "../include/wave_reader.h"
#include "../include/compiled_scene.h"

#ifdef ATG_ENGINE_SIM_PIRANHA_ENABLED
#include "../scripting/include/compiler.h"
#endif

#include <chrono>
#include <stdlib.h>
#include <sstream>
#include <iostream>
#include <cstring>
#include <utility>

#ifdef ATG_ENGINE_SIM_DISCORD_ENABLED
#include "../dependencies/discord/Discord.h"
#endif


namespace {
void releaseObjects(std::vector<SimulationObject *> &objects) {
    for (auto *object : objects) { object->destroy(); delete object; }
    objects.clear();
}

template <typename T>
T *appendObject(std::vector<SimulationObject *> &objects, EngineSimApplication *application) {
    auto release = [](T *object) { static_cast<SimulationObject *>(object)->destroy(); delete object; };
    std::unique_ptr<T, decltype(release)> candidate(new T, release);
    candidate->initialize(application);
    objects.push_back(candidate.get());
    return candidate.release();
}

std::vector<int16_t> readSessionImpulse(const ImpulseResponse &response) {
    try { return engine_sim::readImpulseWave(std::filesystem::u8path(response.getFilename())); }
    catch (const engine_sim::WaveError &error) {
        throw EngineLoadError({"resources", error.path.u8string(), error.reason});
    }
}
}

struct EngineSimApplication::UserInterface {
    UiManager manager;
    EngineView *engineView = nullptr;
    RightGaugeCluster *rightGauge = nullptr;
    OscilloscopeCluster *oscilloscope = nullptr;
    PerformanceCluster *performance = nullptr;
    LoadSimulationCluster *load = nullptr;
    MixerCluster *mixer = nullptr;
    InfoCluster *info = nullptr;
};

struct EngineSimApplication::PendingSession {
    std::unique_ptr<EngineSession> session;
    std::vector<SimulationObject *> objects;
    std::unique_ptr<UserInterface> ui;
    ~PendingSession() {
        if (session != nullptr && session->simulator != nullptr) session->simulator->endAudioRenderingThread();
        ui.reset();
        releaseObjects(objects);
        session.reset();
    }
};

std::string EngineSimApplication::s_buildVersion = "0.1.12a";

EngineSimApplication::EngineSimApplication() {
    m_assetPath = "";


    m_paused = false;
    m_recording = false;
    m_screenResolutionIndex = 0;
    for (int i = 0; i < ScreenResolutionHistoryLength; ++i) {
        m_screenResolution[i][0] = m_screenResolution[i][1] = 0;
    }

    m_background = desktop::color::srgbiToLinear(0x0E1012);
    m_foreground = desktop::color::srgbiToLinear(0xFFFFFF);
    m_shadow = desktop::color::srgbiToLinear(0x0E1012);
    m_highlight1 = desktop::color::srgbiToLinear(0xEF4545);
    m_highlight2 = desktop::color::srgbiToLinear(0xFFFFFF);
    m_pink = desktop::color::srgbiToLinear(0xF394BE);
    m_red = desktop::color::srgbiToLinear(0xEE4445);
    m_orange = desktop::color::srgbiToLinear(0xF4802A);
    m_yellow = desktop::color::srgbiToLinear(0xFDBD2E);
    m_blue = desktop::color::srgbiToLinear(0x77CEE0);
    m_green = desktop::color::srgbiToLinear(0xBDD869);

    m_displayHeight = (float)units::distance(2.0, units::foot);

    m_torque = 0;
    m_dynoSpeed = 0;

    m_simulator = nullptr;
    m_engineView = nullptr;
    m_rightGaugeCluster = nullptr;
    m_temperatureGauge = nullptr;
    m_oscCluster = nullptr;
    m_performanceCluster = nullptr;
    m_loadSimulationCluster = nullptr;
    m_mixerCluster = nullptr;
    m_infoCluster = nullptr;
    m_iceEngine = nullptr;

    m_vehicle = nullptr;
    m_transmission = nullptr;

    m_oscillatorSampleOffset = 0;
    m_gameWindowHeight = 256;
    m_screenWidth = 256;
    m_screenHeight = 256;
    m_screen = 0;
    m_viewParameters.Layer0 = 0;
    m_viewParameters.Layer1 = 0;

    m_displayAngle = 0.0f;
}

EngineSimApplication::~EngineSimApplication() { destroy(); }

void EngineSimApplication::initialize(std::unique_ptr<desktop::Host> host,
        const engine_sim::ResourceOptions &resources) {
    if (m_windowCreated) throw std::logic_error("Application is already initialized");
    if (!host) throw std::invalid_argument("Application requires an explicit desktop host");
    m_destroyed = false;
    m_resources = engine_sim::ResourcePaths::resolve(engine_sim::executablePath(), resources, true);
    m_resources.validateBase();
    const auto scene = engine_sim::readCompiledScene(m_resources.scene());
    m_assetPath = m_resources.assets.u8string();
    m_host = std::move(host);
    m_host->loadModels(scene);
    m_host->initialize(m_resources, scene);
    m_windowCreated = true;
    m_shaders.Initialize(m_host.get());
    m_textRenderer.SetHost(m_host.get());
    m_shaders.SetClearColor(desktop::color::srgbiToLinear(0x34, 0x98, 0xdb));
    m_geometryGenerator.initialize(100000, 200000);
    initialize();
}

void EngineSimApplication::initialize() {
    m_shaders.SetClearColor(desktop::color::srgbiToLinear(0x34, 0x98, 0xdb));
    if (!loadScript()) throw std::runtime_error(m_loadError);

    m_audioBuffer.initialize(44100, 44100);
    m_audioBuffer.m_writePointer = (int)(44100 * 0.1);

    m_host->openAudio();
    m_host->setAudioPlaying(m_simulator->getEngine() != nullptr);

#ifdef ATG_ENGINE_SIM_DISCORD_ENABLED
    // Create a global instance of discord-rpc
    CDiscord::CreateInstance();

    // The optional provider initializes only with an application ID supplied by the operator.
    GetDiscordManager()->SetUseDiscord(true);
    DiscordRichPresence passMe = { 0 };

    std::string engineName = (m_iceEngine != nullptr)
        ? m_iceEngine->getName()
        : "Broken Engine";

    GetDiscordManager()->SetStatus(passMe, engineName, "Independent fork");
#endif /* ATG_ENGINE_SIM_DISCORD_ENABLED */
}

void EngineSimApplication::process(float frame_dt) {
    frame_dt = static_cast<float>(clamp(frame_dt, 1 / 200.0f, 1 / 30.0f));

    double speed = 1.0 / 1.0;
    if (m_host->IsKeyDown(desktop::Key::N1)) {
        speed = 1 / 10.0;
    }
    else if (m_host->IsKeyDown(desktop::Key::N2)) {
        speed = 1 / 100.0;
    }
    else if (m_host->IsKeyDown(desktop::Key::N3)) {
        speed = 1 / 200.0;
    }
    else if (m_host->IsKeyDown(desktop::Key::N4)) {
        speed = 1 / 500.0;
    }
    else if (m_host->IsKeyDown(desktop::Key::N5)) {
        speed = 1 / 1000.0;
    }

    if (m_host->IsKeyDown(desktop::Key::F1)) {
        m_displayAngle += frame_dt * 1.0f;
    }
    else if (m_host->IsKeyDown(desktop::Key::F2)) {
        m_displayAngle -= frame_dt * 1.0f;
    }
    else if (m_host->ProcessKeyDown(desktop::Key::F3)) {
        m_displayAngle = 0.0f;
    }

    m_simulator->setSimulationSpeed(speed);

    const double avgFramerate = clamp(m_host->GetAverageFramerate(), 30.0f, 1000.0f);
    m_simulator->startFrame(1 / avgFramerate);

    auto proc_t0 = std::chrono::steady_clock::now();
    const int iterationCount = m_simulator->getFrameIterationCount();
    while (m_simulator->simulateStep()) {
        m_oscCluster->sample();
    }

    auto proc_t1 = std::chrono::steady_clock::now();

    m_simulator->endFrame();

    auto duration = proc_t1 - proc_t0;
    if (iterationCount > 0) {
        m_performanceCluster->addTimePerTimestepSample(
            (duration.count() / 1E9) / iterationCount);
    }

    const desktop::AudioOffset safeWritePosition = m_host->audioCursor();
    const desktop::AudioOffset writePosition = m_audioBuffer.m_writePointer;

    desktop::AudioOffset targetWritePosition =
        m_audioBuffer.getBufferIndex(safeWritePosition, (int)(44100 * 0.1));
    desktop::AudioOffset maxWrite = m_audioBuffer.offsetDelta(writePosition, targetWritePosition);

    desktop::AudioOffset currentLead = m_audioBuffer.offsetDelta(safeWritePosition, writePosition);
    desktop::AudioOffset newLead = m_audioBuffer.offsetDelta(safeWritePosition, targetWritePosition);

    if (currentLead > 44100 * 0.5) {
        m_audioBuffer.m_writePointer = m_audioBuffer.getBufferIndex(safeWritePosition, (int)(44100 * 0.05));
        currentLead = m_audioBuffer.offsetDelta(safeWritePosition, m_audioBuffer.m_writePointer);
        maxWrite = m_audioBuffer.offsetDelta(m_audioBuffer.m_writePointer, targetWritePosition);
    }

    if (currentLead > newLead) {
        maxWrite = 0;
    }

    maxWrite = std::min<desktop::AudioOffset>(maxWrite, m_audioScratch.size());
    int16_t *samples = m_audioScratch.data();
    const int readSamples = m_simulator->readAudioOutput(maxWrite, samples);

    for (desktop::AudioOffset i = 0; i < (desktop::AudioOffset)readSamples && i < maxWrite; ++i) {
        const int16_t sample = samples[i];
        if (m_oscillatorSampleOffset % 4 == 0) {
            m_oscCluster->getAudioWaveformOscilloscope()->addDataPoint(
                m_oscillatorSampleOffset,
                sample / (float)(INT16_MAX));
        }

        m_audioBuffer.writeSample(sample, m_audioBuffer.m_writePointer, (int)i);

        m_oscillatorSampleOffset = (m_oscillatorSampleOffset + 1) % (44100 / 10);
    }

    if (readSamples > 0) {
        const auto segments = m_host->lockAudio(m_audioBuffer.m_writePointer, readSamples);
        m_audioBuffer.copyBuffer(segments.first, m_audioBuffer.m_writePointer, segments.firstCount);
        m_audioBuffer.copyBuffer(segments.second,
            m_audioBuffer.getBufferIndex(m_audioBuffer.m_writePointer, segments.firstCount), segments.secondCount);
        m_host->unlockAudio(segments);
        m_audioBuffer.commitBlock(readSamples);
    }

    m_performanceCluster->addInputBufferUsageSample(
        (double)m_simulator->getSynthesizerInputLatency() / m_simulator->getSynthesizerInputLatencyTarget());
    m_performanceCluster->addAudioLatencySample(
        m_audioBuffer.offsetDelta(m_host->audioCursor(), m_audioBuffer.m_writePointer) / (44100 * 0.1));
}

void EngineSimApplication::render() {
    for (SimulationObject *object : m_objects) {
        object->generateGeometry();
    }

    m_viewParameters.Sublayer = 0;
    for (SimulationObject *object : m_objects) {
        object->render(&getViewParameters());
    }

    m_viewParameters.Sublayer = 1;
    for (SimulationObject *object : m_objects) {
        object->render(&getViewParameters());
    }

    m_viewParameters.Sublayer = 2;
    for (SimulationObject *object : m_objects) {
        object->render(&getViewParameters());
    }

    m_userInterface->manager.render();
}

float EngineSimApplication::pixelsToUnits(float pixels) const {
    const float f = m_displayHeight / m_engineView->m_bounds.height();
    return pixels * f;
}

float EngineSimApplication::unitsToPixels(float units) const {
    const float f = m_engineView->m_bounds.height() / m_displayHeight;
    return units * f;
}

void EngineSimApplication::run() {
    while (true) {
        m_host->StartFrame();

        if (!m_host->IsOpen()) break;
        if (m_host->ProcessKeyDown(desktop::Key::Escape)) {
            break;
        }

        if (m_host->ProcessKeyDown(desktop::Key::Return)) {
            if (loadScript()) {
                m_host->setAudioPlaying(false);
                m_host->setAudioPlaying(true);
            }
        }

        if (m_host->ProcessKeyDown(desktop::Key::Tab)) {
            m_screen++;
            if (m_screen > 2) m_screen = 0;
        }

        if (m_host->ProcessKeyDown(desktop::Key::F)) {
            if (m_host->GetGameWindow()->GetWindowStyle() != desktop::WindowStyle::Fullscreen) {
                m_host->GetGameWindow()->SetWindowStyle(desktop::WindowStyle::Fullscreen);
                m_infoCluster->setLogMessage("Entered fullscreen mode");
            }
            else {
                m_host->GetGameWindow()->SetWindowStyle(desktop::WindowStyle::Windowed);
                m_infoCluster->setLogMessage("Exited fullscreen mode");
            }
        }

        m_gameWindowHeight = m_host->GetGameWindow()->GetGameHeight();
        m_screenHeight = m_host->GetGameWindow()->GetScreenHeight();
        m_screenWidth = m_host->GetGameWindow()->GetScreenWidth();

        updateScreenSizeStability();

        processEngineInput();

        if (m_host->ProcessKeyDown(desktop::Key::Insert) &&
            m_host->GetGameWindow()->IsActive()) {
            if (!isRecording() && readyToRecord()) {
                startRecording();
            }
            else if (isRecording()) {
                stopRecording();
            }
        }

        if (isRecording() && !readyToRecord()) {
            stopRecording();
        }

        if (!m_paused || m_host->ProcessKeyDown(desktop::Key::Right)) {
            process(m_host->GetFrameLength());
        }

        m_userInterface->manager.update(m_host->GetFrameLength());

        renderScene();

        m_host->EndFrame();

        if (isRecording()) {
            recordFrame();
        }
    }

    if (isRecording()) {
        stopRecording();
    }

    m_simulator->endAudioRenderingThread();
}

void EngineSimApplication::destroy() {
    if (m_destroyed) return;
    m_destroyed = true;
    if (m_session != nullptr) m_session->simulator->endAudioRenderingThread();
    if (m_host) m_host->quiesceAudio();
    m_userInterface.reset();
    destroyObjects();
    m_session.reset();
    bindSession();

    if (m_host) { m_host->shutdown(); m_host.reset(); }
    m_windowCreated = false;
    m_geometryGenerator.destroy();
    m_audioBuffer.destroy();
}

bool EngineSimApplication::loadEngine(Engine *engine, Vehicle *vehicle, Transmission *transmission) {
    EngineDefinition definition;
    definition.engine.reset(engine);
    definition.vehicle.reset(vehicle);
    definition.transmission.reset(transmission);
    definition.applicationSettings = m_applicationSettings;
    return loadEngine(std::move(definition));
}

bool EngineSimApplication::loadEngine(EngineDefinition definition) {
    const auto path = definition.sourcePath;
    try {
        PendingSession candidate;
        candidate.session = EngineSession::prepare(std::move(definition), readSessionImpulse);
        // Widget initialization reads the palette and units. Roll back those values
        // on any failure; window changes are deferred until the accepted swap.
        auto previousSettings = candidate.session->definition.applicationSettings;
        using std::swap;
        swap(m_applicationSettings, previousSettings);
        struct PaletteRollback {
            EngineSimApplication &application;
            ApplicationSettings &previous;
            bool accepted = false;
            ~PaletteRollback() {
                if (!accepted) {
                    using std::swap;
                    swap(application.m_applicationSettings, previous);
                    application.updatePalette();
                }
            }
        } palette{*this, previousSettings};
        updatePalette();

        auto *engine = candidate.session->definition.engine.get();
        auto *simulator = candidate.session->simulator.get();
        buildObjects(engine, candidate.objects);
        candidate.ui = prepareUserInterface(engine, simulator);
        const int maxDepth = engine->getMaxDepth();
        simulator->startAudioRenderingThread();

        if (m_session != nullptr) m_session->simulator->endAudioRenderingThread();
        if (m_host) m_host->quiesceAudio();
        m_session.swap(candidate.session);
        m_objects.swap(candidate.objects);
        m_userInterface.swap(candidate.ui);
        bindSession();
        m_viewParameters.Layer1 = maxDepth;
        palette.accepted = true;
        if (m_applicationSettings.startFullscreen) {
            m_host->GetGameWindow()->SetWindowStyle(desktop::WindowStyle::Fullscreen);
        }
        m_loadError.clear();
        return true;
    }
    catch (const EngineLoadError &error) { reportLoadFailure(error.what()); }
    catch (const std::exception &error) { reportLoadFailure(EngineLoadDiagnostic{"initialize", path, error.what()}.format()); }
    return false;
}

void EngineSimApplication::bindSession() {
    m_iceEngine = m_session ? m_session->definition.engine.get() : nullptr;
    m_vehicle = m_session ? m_session->definition.vehicle.get() : nullptr;
    m_transmission = m_session ? m_session->definition.transmission.get() : nullptr;
    m_simulator = m_session ? m_session->simulator.get() : nullptr;
    m_engineView = m_userInterface ? m_userInterface->engineView : nullptr;
    m_rightGaugeCluster = m_userInterface ? m_userInterface->rightGauge : nullptr;
    m_oscCluster = m_userInterface ? m_userInterface->oscilloscope : nullptr;
    m_performanceCluster = m_userInterface ? m_userInterface->performance : nullptr;
    m_loadSimulationCluster = m_userInterface ? m_userInterface->load : nullptr;
    m_mixerCluster = m_userInterface ? m_userInterface->mixer : nullptr;
    m_infoCluster = m_userInterface ? m_userInterface->info : nullptr;
}

void EngineSimApplication::reportLoadFailure(const std::string &message) {
    m_loadError = message;
    std::cerr << message << '\n';
    if (m_infoCluster != nullptr) m_infoCluster->setLogMessage(message);
}

void EngineSimApplication::drawGenerated(
    const GeometryGenerator::GeometryIndices &indices,
    int layer)
{
    drawGenerated(indices, layer, m_shaders.GetRegularFlags());
}

void EngineSimApplication::drawGeneratedUi(
    const GeometryGenerator::GeometryIndices &indices,
    int layer)
{
    drawGenerated(indices, layer, m_shaders.GetUiFlags());
}

void EngineSimApplication::drawGenerated(
    const GeometryGenerator::GeometryIndices &indices,
    int layer,
    desktop::StageEnableFlags flags)
{
    m_host->drawGenerated(flags, indices.BaseIndex, indices.BaseVertex, indices.FaceCount, layer);
}

void EngineSimApplication::configure(const ApplicationSettings &settings) {
    m_applicationSettings = settings;

    if (settings.startFullscreen) {
        m_host->GetGameWindow()->SetWindowStyle(desktop::WindowStyle::Fullscreen);
    }

    updatePalette();
}

void EngineSimApplication::updatePalette() {
    m_background = desktop::color::srgbiToLinear(m_applicationSettings.colorBackground);
    m_foreground = desktop::color::srgbiToLinear(m_applicationSettings.colorForeground);
    m_shadow = desktop::color::srgbiToLinear(m_applicationSettings.colorShadow);
    m_highlight1 = desktop::color::srgbiToLinear(m_applicationSettings.colorHighlight1);
    m_highlight2 = desktop::color::srgbiToLinear(m_applicationSettings.colorHighlight2);
    m_pink = desktop::color::srgbiToLinear(m_applicationSettings.colorPink);
    m_red = desktop::color::srgbiToLinear(m_applicationSettings.colorRed);
    m_orange = desktop::color::srgbiToLinear(m_applicationSettings.colorOrange);
    m_yellow = desktop::color::srgbiToLinear(m_applicationSettings.colorYellow);
    m_blue = desktop::color::srgbiToLinear(m_applicationSettings.colorBlue);
    m_green = desktop::color::srgbiToLinear(m_applicationSettings.colorGreen);
}

void EngineSimApplication::createObjects(Engine *engine) {
    PendingSession candidate;
    buildObjects(engine, candidate.objects);
    m_objects.swap(candidate.objects);
}

void EngineSimApplication::buildObjects(Engine *engine, std::vector<SimulationObject *> &objects) {
    if (engine == nullptr) throw std::invalid_argument("Cannot build display objects without an engine");
    for (int i = 0; i < engine->getCylinderCount(); ++i) {
        auto *rod = appendObject<ConnectingRodObject>(objects, this);
        rod->m_connectingRod = engine->getConnectingRod(i);
        auto *piston = appendObject<PistonObject>(objects, this);
        piston->m_piston = engine->getPiston(i);
        auto *chamber = appendObject<CombustionChamberObject>(objects, this);
        chamber->m_chamber = engine->getChamber(i);
    }
    for (int i = 0; i < engine->getCrankshaftCount(); ++i) {
        auto *shaft = appendObject<CrankshaftObject>(objects, this);
        shaft->m_crankshaft = engine->getCrankshaft(i);
    }
    for (int i = 0; i < engine->getCylinderBankCount(); ++i) {
        auto *bank = appendObject<CylinderBankObject>(objects, this);
        bank->m_bank = engine->getCylinderBank(i);
        bank->m_head = engine->getHead(i);
        auto *head = appendObject<CylinderHeadObject>(objects, this);
        head->m_head = engine->getHead(i);
        head->m_engine = engine;
    }
}

void EngineSimApplication::destroyObjects() { releaseObjects(m_objects); }

const SimulationObject::ViewParameters &
    EngineSimApplication::getViewParameters() const
{
    return m_viewParameters;
}

bool EngineSimApplication::loadScript() {
#ifdef ATG_ENGINE_SIM_PIRANHA_ENABLED
    try {
        es_script::Compiler compiler;
        compiler.initialize({m_resources.library.u8string(), m_resources.assets.u8string()});
        std::ostringstream diagnostics;
        if (!compiler.compile(m_resources.script.u8string(), diagnostics)) {
            reportLoadFailure(compiler.getLastError().format() + "\n" + diagnostics.str());
            return false;
        }
        return loadEngine(compiler.execute());
    }
    catch (const std::exception &error) { reportLoadFailure(error.what()); }
#else
    reportLoadFailure("compile: ../assets/main.mr: Script support is disabled");
#endif
    return false;
}

void EngineSimApplication::processEngineInput() {
    if (m_iceEngine == nullptr) {
        return;
    }

    const float dt = m_host->GetFrameLength();
    const bool fineControlMode = m_host->IsKeyDown(desktop::Key::Space);

    const int mouseWheel = m_host->GetMouseWheel();
    const int mouseWheelDelta = mouseWheel - m_lastMouseWheel;
    m_lastMouseWheel = mouseWheel;

    bool fineControlInUse = false;
    if (m_host->IsKeyDown(desktop::Key::Z)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.volume = clamp(audioParams.volume + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;

        m_infoCluster->setLogMessage("[Z] - Set volume to " + std::to_string(audioParams.volume));
    }
    else if (m_host->IsKeyDown(desktop::Key::X)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.convolution = clamp(audioParams.convolution + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;

        m_infoCluster->setLogMessage("[X] - Set convolution level to " + std::to_string(audioParams.convolution));
    }
    else if (m_host->IsKeyDown(desktop::Key::C)) {
        const double rate = fineControlMode
            ? 0.00001
            : 0.001;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.dF_F_mix = clamp(audioParams.dF_F_mix + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;

        m_infoCluster->setLogMessage("[C] - Set high freq. gain to " + std::to_string(audioParams.dF_F_mix));
    }
    else if (m_host->IsKeyDown(desktop::Key::V)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.airNoise = clamp(audioParams.airNoise + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;

        m_infoCluster->setLogMessage("[V] - Set low freq. noise to " + std::to_string(audioParams.airNoise));
    }
    else if (m_host->IsKeyDown(desktop::Key::B)) {
        const double rate = fineControlMode
            ? 0.001
            : 0.01;

        Synthesizer::AudioParameters audioParams = m_simulator->synthesizer().getAudioParameters();
        audioParams.inputSampleNoise = clamp(audioParams.inputSampleNoise + mouseWheelDelta * rate * dt);

        m_simulator->synthesizer().setAudioParameters(audioParams);
        fineControlInUse = true;

        m_infoCluster->setLogMessage("[B] - Set high freq. noise to " + std::to_string(audioParams.inputSampleNoise));
    }
    else if (m_host->IsKeyDown(desktop::Key::N)) {
        const double rate = fineControlMode
            ? 10.0
            : 100.0;

        const double newSimulationFrequency = clamp(
            m_simulator->getSimulationFrequency() + mouseWheelDelta * rate * dt,
            400.0, 400000.0);

        m_simulator->setSimulationFrequency(newSimulationFrequency);
        fineControlInUse = true;

        m_infoCluster->setLogMessage("[N] - Set simulation freq to " + std::to_string(m_simulator->getSimulationFrequency()));
    }
    else if (m_host->IsKeyDown(desktop::Key::G) && m_simulator->m_dyno.m_hold) {
        if (mouseWheelDelta > 0) {
            m_dynoSpeed += m_iceEngine->getDynoHoldStep();
        }
        else if (mouseWheelDelta < 0) {
            m_dynoSpeed -= m_iceEngine->getDynoHoldStep();
        }

        m_dynoSpeed = clamp(m_dynoSpeed, m_iceEngine->getDynoMinSpeed(), m_iceEngine->getDynoMaxSpeed());

        m_infoCluster->setLogMessage("[G] - Set dyno speed to " + std::to_string(units::toRpm(m_dynoSpeed)));
        fineControlInUse = true;
    }

    const double prevTargetThrottle = m_targetSpeedSetting;
    m_targetSpeedSetting = fineControlMode ? m_targetSpeedSetting : 0.0;
    if (m_host->IsKeyDown(desktop::Key::Q)) {
        m_targetSpeedSetting = 0.01;
    }
    else if (m_host->IsKeyDown(desktop::Key::W)) {
        m_targetSpeedSetting = 0.1;
    }
    else if (m_host->IsKeyDown(desktop::Key::E)) {
        m_targetSpeedSetting = 0.2;
    }
    else if (m_host->IsKeyDown(desktop::Key::R)) {
        m_targetSpeedSetting = 1.0;
    }
    else if (fineControlMode && !fineControlInUse) {
        m_targetSpeedSetting = clamp(m_targetSpeedSetting + mouseWheelDelta * 0.0001);
    }

    if (prevTargetThrottle != m_targetSpeedSetting) {
        m_infoCluster->setLogMessage("Speed control set to " + std::to_string(m_targetSpeedSetting));
    }

    m_speedSetting = m_targetSpeedSetting * 0.5 + 0.5 * m_speedSetting;

    m_iceEngine->setSpeedControl(m_speedSetting);
    if (m_host->ProcessKeyDown(desktop::Key::M)) {
        const int currentLayer = getViewParameters().Layer0;
        if (currentLayer + 1 < m_iceEngine->getMaxDepth()) {
            setViewLayer(currentLayer + 1);
        }

        m_infoCluster->setLogMessage("[M] - Set render layer to " + std::to_string(getViewParameters().Layer0));
    }

    if (m_host->ProcessKeyDown(desktop::Key::OEM_Comma)) {
        if (getViewParameters().Layer0 - 1 >= 0)
            setViewLayer(getViewParameters().Layer0 - 1);

        m_infoCluster->setLogMessage("[,] - Set render layer to " + std::to_string(getViewParameters().Layer0));
    }

    if (m_host->ProcessKeyDown(desktop::Key::D)) {
        m_simulator->m_dyno.m_enabled = !m_simulator->m_dyno.m_enabled;

        const std::string msg = m_simulator->m_dyno.m_enabled
            ? "DYNOMOMETER ENABLED"
            : "DYNOMOMETER DISABLED";
        m_infoCluster->setLogMessage(msg);
    }

    if (m_host->ProcessKeyDown(desktop::Key::H)) {
        m_simulator->m_dyno.m_hold = !m_simulator->m_dyno.m_hold;

        const std::string msg = m_simulator->m_dyno.m_hold
            ? m_simulator->m_dyno.m_enabled ? "HOLD ENABLED" : "HOLD ON STANDBY [ENABLE DYNO. FOR HOLD]"
            : "HOLD DISABLED";
        m_infoCluster->setLogMessage(msg);
    }

    if (m_simulator->m_dyno.m_enabled) {
        if (!m_simulator->m_dyno.m_hold) {
            if (m_simulator->getFilteredDynoTorque() > units::torque(1.0, units::ft_lb)) {
                m_dynoSpeed += units::rpm(500) * dt;
            }
            else {
                m_dynoSpeed *= (1 / (1 + dt));
            }

            if (m_dynoSpeed > m_iceEngine->getRedline()) {
                m_simulator->m_dyno.m_enabled = false;
                m_dynoSpeed = units::rpm(0);
            }
        }
    }
    else {
        if (!m_simulator->m_dyno.m_hold) {
            m_dynoSpeed = units::rpm(0);
        }
    }

    m_dynoSpeed = clamp(m_dynoSpeed, m_iceEngine->getDynoMinSpeed(), m_iceEngine->getDynoMaxSpeed());
    m_simulator->m_dyno.m_rotationSpeed = m_dynoSpeed;

    const bool prevStarterEnabled = m_simulator->m_starterMotor.m_enabled;
    if (m_host->IsKeyDown(desktop::Key::S)) {
        m_simulator->m_starterMotor.m_enabled = true;
    }
    else {
        m_simulator->m_starterMotor.m_enabled = false;
    }

    if (prevStarterEnabled != m_simulator->m_starterMotor.m_enabled) {
        const std::string msg = m_simulator->m_starterMotor.m_enabled
            ? "STARTER ENABLED"
            : "STARTER DISABLED";
        m_infoCluster->setLogMessage(msg);
    }

    if (m_host->ProcessKeyDown(desktop::Key::A)) {
        m_simulator->getEngine()->getIgnitionModule()->m_enabled =
            !m_simulator->getEngine()->getIgnitionModule()->m_enabled;

        const std::string msg = m_simulator->getEngine()->getIgnitionModule()->m_enabled
            ? "IGNITION ENABLED"
            : "IGNITION DISABLED";
        m_infoCluster->setLogMessage(msg);
    }

    if (m_host->ProcessKeyDown(desktop::Key::Up)) {
        m_simulator->getTransmission()->changeGear(m_simulator->getTransmission()->getGear() + 1);

        m_infoCluster->setLogMessage(
            "UPSHIFTED TO " + std::to_string(m_simulator->getTransmission()->getGear() + 1));
    }
    else if (m_host->ProcessKeyDown(desktop::Key::Down)) {
        m_simulator->getTransmission()->changeGear(m_simulator->getTransmission()->getGear() - 1);

        if (m_simulator->getTransmission()->getGear() != -1) {
            m_infoCluster->setLogMessage(
                "DOWNSHIFTED TO " + std::to_string(m_simulator->getTransmission()->getGear() + 1));
        }
        else {
            m_infoCluster->setLogMessage("SHIFTED TO NEUTRAL");
        }
    }

    if (m_host->IsKeyDown(desktop::Key::T)) {
        m_targetClutchPressure -= 0.2 * dt;
    }
    else if (m_host->IsKeyDown(desktop::Key::U)) {
        m_targetClutchPressure += 0.2 * dt;
    }
    else if (m_host->IsKeyDown(desktop::Key::Shift)) {
        m_targetClutchPressure = 0.0;
        m_infoCluster->setLogMessage("CLUTCH DEPRESSED");
    }
    else if (!m_host->IsKeyDown(desktop::Key::Y)) {
        m_targetClutchPressure = 1.0;
    }

    m_targetClutchPressure = clamp(m_targetClutchPressure);

    double clutchRC = 0.001;
    if (m_host->IsKeyDown(desktop::Key::Space)) {
        clutchRC = 1.0;
    }

    const double clutch_s = dt / (dt + clutchRC);
    m_clutchPressure = m_clutchPressure * (1 - clutch_s) + m_targetClutchPressure * clutch_s;
    m_simulator->getTransmission()->setClutchPressure(m_clutchPressure);
}

void EngineSimApplication::renderScene() {
    getShaders()->ResetBaseColor();
    getShaders()->SetObjectTransform(desktop::math::LoadIdentity());

    m_textRenderer.SetColor(desktop::color::linearToSrgb(m_foreground));
    m_shaders.SetClearColor(desktop::color::linearToSrgb(m_shadow));

    const int screenWidth = m_host->GetGameWindow()->GetGameWidth();
    const int screenHeight = m_host->GetGameWindow()->GetGameHeight();
    const Point cameraPos = m_engineView->getCameraPosition();
    m_shaders.m_cameraPosition = desktop::math::LoadVector(cameraPos.x, cameraPos.y);

    m_shaders.CalculateUiCamera(screenWidth, screenHeight);

    if (m_screen == 0) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        Grid grid;
        grid.v_cells = 2;
        grid.h_cells = 3;
        Grid grid3x3;
        grid3x3.v_cells = 3;
        grid3x3.h_cells = 3;
        m_engineView->setDrawFrame(true);
        m_engineView->setBounds(grid.get(windowBounds, 1, 0, 1, 1));
        m_engineView->setLocalPosition({ 0, 0 });

        m_rightGaugeCluster->m_bounds = grid.get(windowBounds, 2, 0, 1, 2);
        m_oscCluster->m_bounds = grid.get(windowBounds, 1, 1);
        m_performanceCluster->m_bounds = grid3x3.get(windowBounds, 0, 1);
        m_loadSimulationCluster->m_bounds = grid3x3.get(windowBounds, 0, 2);

        Grid grid1x3;
        grid1x3.v_cells = 3;
        grid1x3.h_cells = 1;
        m_mixerCluster->m_bounds = grid1x3.get(grid3x3.get(windowBounds, 0, 0), 0, 2);
        m_infoCluster->m_bounds = grid1x3.get(grid3x3.get(windowBounds, 0, 0), 0, 0, 1, 2);

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(true);
        m_oscCluster->setVisible(true);
        m_performanceCluster->setVisible(true);
        m_loadSimulationCluster->setVisible(true);
        m_mixerCluster->setVisible(true);
        m_infoCluster->setVisible(true);

        m_oscCluster->activate();
    }
    else if (m_screen == 1) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        m_engineView->setDrawFrame(false);
        m_engineView->setBounds(windowBounds);
        m_engineView->setLocalPosition({ 0, 0 });
        m_engineView->activate();

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(false);
        m_oscCluster->setVisible(false);
        m_performanceCluster->setVisible(false);
        m_loadSimulationCluster->setVisible(false);
        m_mixerCluster->setVisible(false);
        m_infoCluster->setVisible(false);
    }
    else if (m_screen == 2) {
        Bounds windowBounds((float)screenWidth, (float)screenHeight, { 0, (float)screenHeight });
        Grid grid;
        grid.v_cells = 1;
        grid.h_cells = 3;
        m_engineView->setDrawFrame(true);
        m_engineView->setBounds(grid.get(windowBounds, 0, 0, 2, 1));
        m_engineView->setLocalPosition({ 0, 0 });
        m_engineView->activate();

        m_rightGaugeCluster->m_bounds = grid.get(windowBounds, 2, 0, 1, 1);

        m_engineView->setVisible(true);
        m_rightGaugeCluster->setVisible(true);
        m_oscCluster->setVisible(false);
        m_performanceCluster->setVisible(false);
        m_loadSimulationCluster->setVisible(false);
        m_mixerCluster->setVisible(false);
        m_infoCluster->setVisible(false);
    }

    const float cameraAspectRatio =
        m_engineView->m_bounds.width() / m_engineView->m_bounds.height();
    m_host->setSceneViewport({
        static_cast<int>(m_engineView->m_bounds.getPosition(Bounds::tl).x),
        static_cast<int>(screenHeight - m_engineView->m_bounds.getPosition(Bounds::tl).y),
        static_cast<int>(m_engineView->m_bounds.width()), static_cast<int>(m_engineView->m_bounds.height())});
    m_shaders.CalculateCamera(
        cameraAspectRatio * m_displayHeight / m_engineView->m_zoom,
        m_displayHeight / m_engineView->m_zoom,
        m_engineView->m_bounds,
        m_screenWidth,
        m_screenHeight,
        m_displayAngle);

    m_geometryGenerator.reset();

    render();

    m_host->uploadGeometry(m_geometryGenerator.getVertexData(), m_geometryGenerator.getCurrentVertexCount(),
        m_geometryGenerator.getIndexData(), m_geometryGenerator.getCurrentIndexCount());
}

std::unique_ptr<EngineSimApplication::UserInterface>
EngineSimApplication::prepareUserInterface(Engine *engine, Simulator *simulator) {
    if (engine == nullptr || simulator == nullptr) throw std::invalid_argument("Cannot build interface without an engine session");
    auto candidate = std::make_unique<UserInterface>();
    candidate->manager.initialize(this);
    auto *root = candidate->manager.getRoot();
    candidate->engineView = root->addElement<EngineView>();
    candidate->rightGauge = root->addElement<RightGaugeCluster>();
    candidate->oscilloscope = root->addElement<OscilloscopeCluster>();
    candidate->performance = root->addElement<PerformanceCluster>();
    candidate->load = root->addElement<LoadSimulationCluster>();
    candidate->mixer = root->addElement<MixerCluster>();
    candidate->info = root->addElement<InfoCluster>();
    candidate->info->setEngine(engine);
    candidate->rightGauge->m_simulator = simulator;
    candidate->rightGauge->setEngine(engine);
    candidate->oscilloscope->setSimulator(simulator);
    candidate->oscilloscope->setDynoMaxRange(units::toRpm(engine->getRedline()));
    candidate->performance->setSimulator(simulator);
    candidate->load->setSimulator(simulator);
    candidate->mixer->setSimulator(simulator);
    return candidate;
}

void EngineSimApplication::refreshUserInterface() {
    auto candidate = prepareUserInterface(m_iceEngine, m_simulator);
    m_userInterface.swap(candidate);
    bindSession();
}

void EngineSimApplication::startRecording() {
    m_recording = true;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    atg_dtv::Encoder::VideoSettings settings{};

    // Output filename
    settings.fname = "../workspace/video_capture/engine_sim_video_capture.mp4";
    settings.inputWidth = m_host->GetScreenWidth();
    settings.inputHeight = m_host->GetScreenHeight();
    settings.width = settings.inputWidth;
    settings.height = settings.inputHeight;
    settings.hardwareEncoding = true;
    settings.inputAlpha = true;
    settings.bitRate = 40000000;

    m_encoder.run(settings, 2);
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}

void EngineSimApplication::updateScreenSizeStability() {
    m_screenResolution[m_screenResolutionIndex][0] = m_host->GetScreenWidth();
    m_screenResolution[m_screenResolutionIndex][1] = m_host->GetScreenHeight();

    m_screenResolutionIndex = (m_screenResolutionIndex + 1) % ScreenResolutionHistoryLength;
}

bool EngineSimApplication::readyToRecord() {
    const int w = m_screenResolution[0][0];
    const int h = m_screenResolution[0][1];

    if (w <= 0 && h <= 0) return false;
    if ((w % 2) != 0 || (h % 2) != 0) return false;

    for (int i = 1; i < ScreenResolutionHistoryLength; ++i) {
        if (m_screenResolution[i][0] != w) return false;
        if (m_screenResolution[i][1] != h) return false;
    }

    return true;
}

void EngineSimApplication::stopRecording() {
    m_recording = false;

#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    m_encoder.commit();
    m_encoder.stop();
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}

void EngineSimApplication::recordFrame() {
#ifdef ATG_ENGINE_SIM_VIDEO_CAPTURE
    atg_dtv::Frame *frame = m_encoder.newFrame(false);
    if (frame != nullptr && m_encoder.getError() == atg_dtv::Encoder::Error::None) {
        m_host->readScreen(frame->m_rgb, static_cast<std::size_t>(m_host->GetScreenWidth()) * m_host->GetScreenHeight() * 4);
    }

    m_encoder.submitFrame();
#endif /* ATG_ENGINE_SIM_VIDEO_CAPTURE */
}
