#include <gtest/gtest.h>
#include "constants.h"
#include "piston_object.h"
#include "connecting_rod_object.h"
#include "cylinder_bank_object.h"
#include "cylinder_head_object.h"
#include "engine_sim_application.h"
#include "recording_host.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <thread>
#include <type_traits>

namespace {
namespace fs=std::filesystem;
using desktop::Key;
using host_test::Input;
class Application : public EngineSimApplication {
public:
    using EngineSimApplication::setMixer;
    bool failViews=false;
    void viewHeight(float height) { m_engineView->setBounds(Bounds(1280,height,{0,height})); }
    desktop::EngineViews &views() { return *m_engineViews; }
    Synthesizer::AudioParameters audioParameters() {
        auto lease = views().present();
        return views().at<Synthesizer>()->getAudioParameters();
    }
    Synthesizer::BufferStatistics audioStatistics() {
        auto lease = views().present();
        return views().at<Synthesizer>()->getBufferStatistics();
    }
    desktop::EngineCommands &commands() { return *m_desktopControls; }
    engine_sim::PullEngine &pull() { return *m_pullEngine; }
    int processed=0;
    std::function<void()> beforeProcess;
    void pause(bool value) { m_paused=value; }
    int screen() const { return m_screen; }
    float zoom() const { return m_engineView->m_zoom; }
    Point camera() const { return m_engineView->getCameraPosition(); }
    double throttle() const { return m_targetSpeedSetting; }
    double clutch() const { return m_clutchPressure; }
private:
    void validatePreparedViews() override { if (failViews) throw std::runtime_error("Injected detached view preparation failure"); }
    void process(float seconds) override {
        ++processed;
        if (beforeProcess) beforeProcess();
        EngineSimApplication::process(seconds);
    }
};
class HostApplication : public testing::Test {
protected:
    void SetUp() override {
        const auto *value=std::getenv("ENGINE_SIM_TEST_RESOURCES"); ASSERT_NE(value,nullptr);
        resources=fs::canonical(fs::u8path(value));
        const auto directory=fs::canonical(fs::current_path());
        ASSERT_NE(directory,resources);
        const auto relative=directory.lexically_relative(resources);
        ASSERT_TRUE(relative.empty() || *relative.begin()=="..");
        scratch=directory/(std::string("host-")+testing::UnitTest::GetInstance()->current_test_info()->name()
            +"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directory(scratch);
        std::srand(1);
    }
    std::unique_ptr<host_test::RecordingHost> host(std::size_t frames) {
        recording=std::make_shared<host_test::Recording>();
        auto value=std::make_unique<host_test::RecordingHost>(recording);
        value->outputDirectory=scratch/("recording-"+std::to_string(sequence++));
        fs::create_directory(value->outputDirectory);
        value->inputs.resize(frames); return value;
    }
    std::string readModel(const fs::path &name) {
        std::ifstream stream(resources / name, std::ios::binary);
        if (!stream) throw std::runtime_error("Missing canonical desktop test model");
        std::ostringstream data;
        data << stream.rdbuf();
        return data.str();
    }
    fs::path entry(const std::string &text) {
        const auto file=scratch/fs::u8path("custom entry \xCE\xA9.json"); write(file,text); return file;
    }
    void write(const fs::path &file,const std::string &text) {
        std::ofstream stream(file,std::ios::binary); stream<<text; ASSERT_TRUE(stream);
    }
    static bool model(const host_test::RecordedFrame &frame,const std::string &name) {
        return std::any_of(frame.packets.begin(),frame.packets.end(),[&](const auto &packet) { return packet.model==name; });
    }
    static bool text(const host_test::RecordedFrame &frame,const std::string &name) {
        return std::any_of(frame.packets.begin(),frame.packets.end(),[&](const auto &packet) { return packet.text==name; });
    }
    fs::path resources,scratch;
    std::shared_ptr<host_test::Recording> recording;
    int sequence=0;
};

TEST_F(HostApplication, RequiresAnExplicitHostAndValidResourceRoot) {
    Application app;
    EXPECT_THROW(app.initialize(nullptr,{resources,{}}),std::invalid_argument);
    auto backend=host(1);
    EXPECT_THROW(app.initialize(std::move(backend),{scratch/"absent",{}}),engine_sim::ResourceError);
    EXPECT_EQ(recording->initialized,0); EXPECT_EQ(recording->shutdowns,1);
    app.destroy(); EXPECT_EQ(app.getSimulator(),nullptr);
}

TEST_F(HostApplication, DefaultGraphSubmitsModelsAndAllThreeScreens) {
    GeometryGenerator geometry; geometry.initialize(16,24); geometry.startShape();
    ASSERT_TRUE(geometry.generateLine2d({1,2,1,2,0.1f}));
    GeometryGenerator::GeometryIndices empty; geometry.endShape(&empty);
    EXPECT_EQ(empty.FaceCount,0); EXPECT_EQ(geometry.getCurrentVertexCount(),0); EXPECT_EQ(geometry.getCurrentIndexCount(),0);
    ASSERT_TRUE(geometry.generateLine2d({1,2,2,2,0.1f}));
    EXPECT_EQ(geometry.getCurrentVertexCount(),4); EXPECT_EQ(geometry.getCurrentIndexCount(),6);
    EXPECT_FLOAT_EQ(geometry.getVertexData()[0].Pos.x,1); EXPECT_FLOAT_EQ(geometry.getVertexData()[0].Pos.y,2.05f);
    for (bool constant : {false,true}) {
        geometry.reset(); geometry.startShape();
        Point points[]{{0,0},{0,0},{1,0},{2,0}};
        if (constant) for (auto &point : points) point={0,0};
        GeometryGenerator::PathParameters path{}; path.p0=points; path.n0=4; path.width=1;
        ASSERT_TRUE(geometry.startPath(path));
        for (int i=1; i<4; ++i) { path.i=i; ASSERT_TRUE(geometry.generatePathSegment(path,false)); }
        for (int i=0; i<geometry.getCurrentVertexCount(); ++i) {
            EXPECT_TRUE(std::isfinite(geometry.getVertexData()[i].Pos.x));
            EXPECT_TRUE(std::isfinite(geometry.getVertexData()[i].Pos.y));
        }
    }
    Application app; auto backend=host(4);
    backend->inputs[1].press(Key::Tab); backend->inputs[3].press(Key::Tab);
    app.initialize(std::move(backend),{resources,{}});
    Oscilloscope scope; scope.initialize(&app);
    const Bounds bounds(100,60,{0,0});
    const auto constantPosition=scope.dataPointToRenderPosition({0,0},bounds);
    scope.m_xMin=scope.m_yMin=-1; scope.m_xMax=scope.m_yMax=1;
    const auto center=scope.dataPointToRenderPosition({0,0},bounds);
    EXPECT_FLOAT_EQ(constantPosition.x,center.x); EXPECT_FLOAT_EQ(constantPosition.y,center.y);
    scope.destroy();
    ASSERT_EQ(app.getSimulator()->getEngine()->getCylinderCount(),4);
    app.run(); ASSERT_EQ(recording->frames.size(),4U); EXPECT_EQ(app.screen(),2);
    const auto &first=recording->frames[0], &second=recording->frames[1], &third=recording->frames[3];
    for (const auto *name : {"Logo","Crankshaft","CrankSnout","CrankSnoutThreads","Piston","ConnectingRod","CylinderHead","Valve"}) {
        EXPECT_TRUE(model(first,name))<<name;
    }
    EXPECT_TRUE(text(first,"ENGINE SIMULATOR")); EXPECT_TRUE(text(first,"Original simulator by Ange Yaghi"));
    EXPECT_FALSE(model(second,"Logo")); EXPECT_FALSE(model(third,"Logo"));
    const auto sceneWidth=[](const host_test::RecordedFrame &frame) {
        for (const auto &packet : frame.packets) if (packet.stage==desktop::SceneStage) return packet.state.viewport.width;
        return 0;
    };
    EXPECT_GT(sceneWidth(first),0); EXPECT_LT(sceneWidth(first),sceneWidth(third)); EXPECT_LT(sceneWidth(third),sceneWidth(second));
    for (const auto &frame : recording->frames) { EXPECT_GT(frame.vertices,0U); EXPECT_GT(frame.indices,0U); }
    app.destroy(); EXPECT_EQ(recording->shutdowns,1);
}

TEST_F(HostApplication, PausedLoopConsumesSingleStepEdgesWithoutRepeatingHolds) {
    Application app; auto backend=host(3);
    backend->inputs[0].press(Key::Right); backend->inputs[1].hold(Key::Right); backend->inputs[2].press(Key::Right);
    app.initialize(std::move(backend),{resources,{}}); app.pause(true); app.run();
    EXPECT_EQ(app.processed,2); EXPECT_EQ(recording->frames.size(),3U);
    app.destroy(); EXPECT_EQ(recording->shutdowns,1);
}

TEST_F(HostApplication, ControlsPreserveEdgesHoldsClutchThrottleAndFineWheel) {
    Application app; auto backend=host(8);
    backend->inputs[0].press(Key::A).hold(Key::S).press(Key::D).press(Key::H).press(Key::Up).hold(Key::R).hold(Key::Shift);
    backend->inputs[1].hold(Key::A).hold(Key::S).hold(Key::D).hold(Key::H).hold(Key::Up);
    backend->inputs[3].press(Key::A).press(Key::Down);
    backend->inputs[4].press(Key::M); backend->inputs[5].press(Key::OEM_Comma);
    backend->inputs[6].hold(Key::Z); backend->inputs[6].wheel=120;
    backend->inputs[7].hold(Key::Z).hold(Key::Space); backend->inputs[7].wheel=240;
    app.initialize(std::move(backend),{resources,{}});
    auto parameters=app.audioParameters(); parameters.volume=0.25;
    app.setMixer(parameters);
    const auto layer=app.getViewParameters().Layer0; ASSERT_GT(app.getSimulator()->getEngine()->getMaxDepth(),1);
    struct Snapshot { bool starter,ignition,dyno,hold; int gear,layer; double throttle,clutch; };
    std::vector<Snapshot> snapshots;
    app.beforeProcess=[&] {
        auto *sim=app.getSimulator();
        snapshots.push_back({sim->m_starterMotor.m_enabled,sim->getEngine()->getIgnitionModule()->m_enabled,
            sim->m_dyno.m_enabled,sim->m_dyno.m_hold,sim->getTransmission()->getGear(),app.getViewParameters().Layer0,
            app.throttle(),app.clutch()});
    };
    app.run(); ASSERT_EQ(snapshots.size(),8U);
    EXPECT_TRUE(snapshots[0].starter); EXPECT_TRUE(snapshots[0].ignition); EXPECT_TRUE(snapshots[0].dyno); EXPECT_TRUE(snapshots[0].hold);
    EXPECT_EQ(snapshots[0].gear,0); EXPECT_DOUBLE_EQ(snapshots[0].throttle,1); EXPECT_LT(snapshots[0].clutch,0.1);
    EXPECT_TRUE(snapshots[1].ignition); EXPECT_TRUE(snapshots[1].dyno); EXPECT_EQ(snapshots[1].gear,0);
    EXPECT_FALSE(snapshots[2].starter); EXPECT_FALSE(snapshots[3].ignition); EXPECT_EQ(snapshots[3].gear,-1);
    EXPECT_EQ(snapshots[4].layer,layer+1); EXPECT_EQ(snapshots[5].layer,layer);
    EXPECT_NEAR(app.audioParameters().volume,0.272,1e-6);
    app.destroy();
}

TEST_F(HostApplication, ResizeFullscreenWheelAndMouseKeepLogicalCoordinates) {
    Application app; auto backend=host(6);
    for (std::size_t i=0; i<backend->inputs.size(); ++i) {
        auto &input=backend->inputs[i]; const int width=i<2?1280:1600,height=i<2?720:900;
        input.frame.window={width,height,width,height,width*2,height*2,true,true,false};
        input.mouseX=width/2; input.mouseY=height*3/4; input.wheel=i==0?0:120;
    }
    backend->inputs[2].press(Key::F); backend->inputs[3].mouseDown=true;
    backend->inputs[4].mouseX+=80; backend->inputs[4].mouseY+=30;
    backend->inputs[5].mouseUp=true; backend->inputs[5].press(Key::F);
    app.initialize(std::move(backend),{resources,{}}); const auto initial=app.camera(); app.run();
    ASSERT_EQ(recording->frames.size(),6U); EXPECT_EQ(recording->fullscreen,(std::vector<bool>{true,false}));
    EXPECT_EQ(recording->frames[0].frame.window.gameWidth,1280); EXPECT_EQ(recording->frames[2].frame.window.gameWidth,1600);
    EXPECT_EQ(recording->frames[2].frame.window.framebufferWidth,3200); EXPECT_EQ(app.getScreenWidth(),1600);
    EXPECT_GT(app.zoom(),1); EXPECT_TRUE(app.camera().x!=initial.x || app.camera().y!=initial.y);
    app.destroy();
}

TEST_F(HostApplication, FailedReturnReloadKeepsSessionAndSuccessfulReturnReplacesIt) {
    const auto valid = readModel("assets/engines/atg-video-2/01_subaru_ej25_eh.json");
    const auto modelFile=entry(valid); Application app; auto backend=host(4); auto *control=backend.get();
    backend->inputs[1].press(Key::Return); backend->inputs[2].press(Key::Return);
    app.initialize(std::move(backend),{resources, {}, modelFile}); auto *original=app.getSimulator();
    control->onFrame=[&](std::size_t frame) {
        if (frame==1) write(modelFile, "{\"components\":");
        if (frame==2) { EXPECT_EQ(app.getSimulator(),original); write(modelFile, valid); }
        if (frame==3) EXPECT_NE(app.getSimulator(),original);
    };
    app.run(); EXPECT_EQ(recording->audioModes,(std::vector<bool>{true,false,true}));
    ASSERT_EQ(recording->frames.size(),4U); EXPECT_TRUE(model(recording->frames[1],"Piston"));
    app.destroy(); EXPECT_EQ(recording->shutdowns,1);
}

TEST_F(HostApplication, PartialStartupAndAudioFailureRetireSessionBeforeHost) {
    for (bool audioFailure : {false,true}) {
        Application app; auto backend=host(1); auto data=recording;
        backend->failInitialize=!audioFailure; backend->failAudio=audioFailure;
        data->onShutdown=[&] { EXPECT_EQ(app.getSimulator(),nullptr); };
        EXPECT_THROW(app.initialize(std::move(backend),{resources,{}}),desktop::DesktopError);
        app.destroy(); EXPECT_EQ(data->shutdowns,1); EXPECT_EQ(data->audioOpened,audioFailure?1:0);
        app.destroy(); EXPECT_EQ(data->shutdowns,1);
    }
}

TEST_F(HostApplication, KohlerAndNineCylinderSessionsRenderProducePcmAndRelease) {
    const std::array<std::pair<const char *,int>,3> engines{{
        {"assets/engines/atg-video-1/02_kohler_ch750.json",2},
        {"assets/engines/atg-video-2/09_radial_9.json",9},
        {"assets/engines/atg-video-2/09_radial_9.json",9}}};
    for (const auto &engine : engines) {
        SCOPED_TRACE(engine.first);
        const auto modelFile = resources / engine.first;
        Application app; auto backend=host(40); auto data=recording;
        for (auto &input : backend->inputs) input.hold(Key::S);
        backend->inputs[0].press(Key::A);
        app.initialize(std::move(backend),{resources, {}, modelFile});
        ASSERT_EQ(app.getSimulator()->getEngine()->getCylinderCount(),engine.second);
        data->onShutdown=[&] { EXPECT_EQ(app.getSimulator(),nullptr); };
        app.run(); EXPECT_EQ(data->frames.size(),40U); EXPECT_GT(data->pcm.size(),0U);
        EXPECT_TRUE(std::any_of(data->pcm.begin(),data->pcm.end(),[](std::int16_t value) { return value!=0; }));
        EXPECT_TRUE(model(data->frames.back(),"Piston"));
        app.destroy(); EXPECT_EQ(data->shutdowns,1);
    }
}

class ViewsApplication : public HostApplication {};

TEST_F(ViewsApplication, HandlesRequireOwnerPhaseAndRetireBeforeTheirGraph) {
    static_assert(std::is_same_v<decltype(std::declval<desktop::ViewHandle<Engine>>().operator->()), const Engine *>);
    static_assert(std::is_same_v<decltype(std::declval<const Engine>().getPiston(0)), const Piston *>);
    static_assert(std::is_same_v<decltype(std::declval<const Piston>().getRod()), const ConnectingRod *>);
    static_assert(std::is_same_v<decltype(std::declval<const Simulator>().getEngine()), const Engine *>);
    Application app; app.initialize(host(1),{resources,{}});
    auto old=app.engineObservation(); auto piston=app.views().at<Piston>(0);
    EXPECT_THROW(old->getRpm(),std::logic_error);
    {
        auto phase=app.presentation();
        EXPECT_EQ(old->getCylinderCount(),4); EXPECT_GE(piston->getCylinderIndex(),0);
        EXPECT_THROW(app.commands().flush(),std::logic_error);
        bool rejected=false;
        std::thread other([&] { try { (void)old->getRpm(); } catch (const std::logic_error &) { rejected=true; } });
        other.join(); EXPECT_TRUE(rejected);
    }
    EXPECT_THROW(piston->getCylinderIndex(),std::logic_error);
    auto replacement = [&] {
        auto lease = app.presentation();
        const auto *synthesis = app.views().at<Synthesizer>().operator->();
        return desktop::EngineViews(*app.getSimulator(), *synthesis);
    }();
    app.views().invalidate();
    auto fresh=replacement.at<Engine>();
    EXPECT_NE(old.generation(),fresh.generation());
    {
        auto phase=replacement.present(); EXPECT_EQ(fresh->getCylinderCount(),4);
        EXPECT_THROW(old->getCylinderCount(),std::logic_error);
    }
    app.destroy(); EXPECT_FALSE(old.valid()); EXPECT_FALSE(piston.valid());
    EXPECT_THROW(old->getRpm(),std::logic_error);
}

TEST_F(ViewsApplication, FailedDetachedViewsKeepPaletteGenerationControlsAndDrawableGeometry) {
    const auto valid = readModel("assets/engines/atg-video-2/01_subaru_ej25_eh.json");
    const auto modelFile=entry(valid); Application app; auto backend=host(4); auto *control=backend.get();
    backend->inputs[0].press(Key::A).hold(Key::S);
    backend->inputs[1].press(Key::Return); backend->inputs[2].press(Key::Return);
    app.initialize(std::move(backend),{resources, {}, modelFile});
    auto old=app.engineObservation(); const auto generation=app.viewGeneration();
    const auto palette=app.getAppSettings()->colorForeground;
    control->onFrame=[&](std::size_t frame) {
        if (frame==1) {
            auto changed = valid;
            const std::string property = "\"color_foreground\":16777215";
            const auto at = changed.find(property);
            ASSERT_NE(at, std::string::npos);
            changed.replace(at, property.size(), "\"color_foreground\":1193046");
            write(modelFile, changed);
            app.failViews=true;
        }
        if (frame==2) {
            EXPECT_EQ(app.viewGeneration(),generation); EXPECT_EQ(app.getAppSettings()->colorForeground,palette);
            auto phase=app.presentation(); EXPECT_TRUE(old->getIgnitionModule()->m_enabled);
            EXPECT_EQ(old->getCylinderCount(),4); app.failViews=false;
        }
        if (frame==3) {
            EXPECT_NE(app.viewGeneration(),generation); EXPECT_FALSE(old.valid());
            EXPECT_EQ(app.getAppSettings()->colorForeground,0x123456);
            auto phase=app.presentation(); EXPECT_THROW(old->getRpm(),std::logic_error);
        }
    };
    app.run(); ASSERT_EQ(recording->frames.size(),4U);
    EXPECT_TRUE(model(recording->frames[1],"Piston")); EXPECT_TRUE(model(recording->frames[3],"Piston"));
    EXPECT_EQ(recording->audioModes,(std::vector<bool>{true,false,true}));
    app.destroy();
}

TEST_F(ViewsApplication, TickObservationsMatchModelAndEvictOnlyOldDisplayPoints) {
    Application app; app.initialize(host(1),{resources,{}});
    auto &views=app.views(); auto &pull=app.pull();
    std::vector<desktop::ScopeObservation> expected;
    pull.setTickObserver([&](const Simulator &sim) {
        views.sampleTick(sim);
        if (sim.getCurrentIteration()%2) return;
        const auto *engine=sim.getEngine(); const auto *chamber=engine->getChamber(0);
        auto cycle=engine->getCrankshaft(0)->getCycleAngle();
        if (!engine->isSpinningCw()) cycle=4*constants::pi-cycle;
        expected.push_back({views.observedTicks(),cycle,engine->getCrankshaft(0)->getCycleAngle(constants::pi),
            sim.getTotalExhaustFlow()/sim.getTimestep(),
            std::sqrt(chamber->m_system.pressure()+chamber->m_system.dynamicPressure(-1,0)),
            chamber->getLastTimestepExhaustFlow()/sim.getTimestep(),
            chamber->getLastTimestepIntakeFlow()/sim.getTimestep(),chamber->m_system.n(),
            chamber->getCylinderHead()->exhaustValveLift(chamber->getPiston()->getCylinderIndex()),
            chamber->getCylinderHead()->intakeValveLift(chamber->getPiston()->getCylinderIndex()),
            chamber->getVolume(),std::sqrt(chamber->m_system.pressure()),units::toRpm(engine->getSpeed())});
    });
    app.commands().setStarter(true); app.commands().setIgnition(true); app.commands().flush();
    std::array<float,257> pcm{};
    for (int i=0;i<400;++i) pull.render(pcm.data(),pcm.size());
    ASSERT_GT(expected.size(),desktop::EngineViews::ScopeCapacity);
    const auto first=expected.size()-desktop::EngineViews::ScopeCapacity;
    EXPECT_EQ(views.evictedScopePoints(),first);
    desktop::ScopeObservation point{}; std::size_t i=first;
    while (views.popScope(point)) {
        ASSERT_LT(i,expected.size()); const auto &reference=expected[i++];
        EXPECT_EQ(point.tick,reference.tick); EXPECT_DOUBLE_EQ(point.cycle,reference.cycle);
        EXPECT_DOUBLE_EQ(point.pressureCycle,reference.pressureCycle); EXPECT_DOUBLE_EQ(point.pressure,reference.pressure);
        EXPECT_DOUBLE_EQ(point.exhaust,reference.exhaust); EXPECT_DOUBLE_EQ(point.cylinderExhaust,reference.cylinderExhaust);
        EXPECT_DOUBLE_EQ(point.intake,reference.intake); EXPECT_DOUBLE_EQ(point.molecules,reference.molecules);
        EXPECT_DOUBLE_EQ(point.exhaustLift,reference.exhaustLift); EXPECT_DOUBLE_EQ(point.intakeLift,reference.intakeLift);
        EXPECT_DOUBLE_EQ(point.volume,reference.volume); EXPECT_DOUBLE_EQ(point.staticPressure,reference.staticPressure);
    }
    EXPECT_EQ(i,expected.size()); EXPECT_EQ(app.sourceCounts().frames,400U*257);
    const auto audio = app.audioStatistics();
    EXPECT_EQ(audio.droppedInput, 0U);
    EXPECT_EQ(audio.droppedOutput, 0U);
    app.destroy();
}


TEST_F(ViewsApplication, StaticGeometryCacheMatchesFreshGeometryAfterOffsetsAndResize) {
    Application app; app.initialize(host(1),{resources,{}});
    auto phase=app.presentation();
    PistonObject piston; piston.initialize(&app); piston.m_piston=app.views().at<Piston>(0);
    ConnectingRodObject rod; rod.initialize(&app); rod.m_connectingRod=app.views().at<ConnectingRod>(0);
    CylinderBankObject bank; bank.initialize(&app); bank.m_bank=app.views().at<CylinderBank>(0);
    bank.m_head=app.views().at<CylinderHead>(0);
    CylinderHeadObject head; head.initialize(&app); head.m_head=app.views().at<CylinderHead>(0);
    auto *generator=app.getGeometryGenerator();
    for (auto *object : std::array<SimulationObject *,4>{&piston,&rod,&bank,&head}) {
        for (float height : {360.0f,720.0f,720.0f}) {
            app.viewHeight(height);
            generator->reset(); object->generateGeometry();
            const auto vertices=generator->getCurrentVertexCount(), indices=generator->getCurrentIndexCount();
            std::vector<desktop::Vector4> positions;
            for (int i=0;i<vertices;++i) positions.push_back(generator->getVertexData()[i].Pos);
            std::vector<unsigned short> faces(generator->getIndexData(),generator->getIndexData()+indices);
            generator->reset();
            GeometryGenerator::Circle2dParameters prefix; prefix.radius=1; prefix.maxEdgeLength=0.25f;
            generator->startShape(); generator->generateCircle2d(prefix);
            const int vertexBase=generator->getCurrentVertexCount(), indexBase=generator->getCurrentIndexCount();
            object->generatePresentationGeometry();
            ASSERT_EQ(generator->getCurrentVertexCount()-vertexBase,vertices);
            ASSERT_EQ(generator->getCurrentIndexCount()-indexBase,indices);
            for (int i=0;i<vertices;++i) {
                const auto &actual=generator->getVertexData()[vertexBase+i].Pos;
                EXPECT_FLOAT_EQ(actual.x,positions[i].x); EXPECT_FLOAT_EQ(actual.y,positions[i].y);
                EXPECT_FLOAT_EQ(actual.z,positions[i].z); EXPECT_FLOAT_EQ(actual.w,positions[i].w);
            }
            for (int i=0;i<indices;++i) EXPECT_EQ(generator->getIndexData()[indexBase+i],faces[i]);
        }
    }
    // The lease deliberately ends before app's destructor retires its owner.
}

}
