#include <gtest/gtest.h>
#include "engine_sim_application.h"
#include "recording_host.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

namespace {
namespace fs=std::filesystem;
using desktop::Key;
using host_test::Input;
class Application : public EngineSimApplication {
public:
    int processed=0;
    std::function<void()> beforeProcess;
    void pause(bool value) { m_paused=value; }
    int screen() const { return m_screen; }
    float zoom() const { return m_engineView->m_zoom; }
    Point camera() const { return m_engineView->getCameraPosition(); }
    double throttle() const { return m_targetSpeedSetting; }
    double clutch() const { return m_clutchPressure; }
private:
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
    fs::path entry(const std::string &text) {
        const auto file=scratch/fs::u8path("custom entry \xCE\xA9.mr"); write(file,text); return file;
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
    auto parameters=app.getSimulator()->synthesizer().getAudioParameters(); parameters.volume=0.25;
    app.getSimulator()->synthesizer().setAudioParameters(parameters);
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
    EXPECT_NEAR(app.getSimulator()->synthesizer().getAudioParameters().volume,0.272,1e-6);
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
    const std::string valid="import \"engine_sim.mr\"\nimport \"engines/atg-video-2/01_subaru_ej25_eh.mr\"\nmain()\n";
    const auto script=entry(valid); Application app; auto backend=host(4); auto *control=backend.get();
    backend->inputs[1].press(Key::Return); backend->inputs[2].press(Key::Return);
    app.initialize(std::move(backend),{resources,script}); auto *original=app.getSimulator();
    control->onFrame=[&](std::size_t frame) {
        if (frame==1) write(script,"import \"engine_sim.mr\"\nmissing_contract_node()\n");
        if (frame==2) { EXPECT_EQ(app.getSimulator(),original); write(script,valid); }
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
        {"engines/atg-video-1/02_kohler_ch750.mr",2},
        {"engines/atg-video-2/09_radial_9.mr",9},
        {"engines/atg-video-2/09_radial_9.mr",9}}};
    for (const auto &engine : engines) {
        SCOPED_TRACE(engine.first);
        const auto script=entry("import \"engine_sim.mr\"\nimport \""+std::string(engine.first)+"\"\nmain()\n");
        Application app; auto backend=host(40); auto data=recording;
        for (auto &input : backend->inputs) input.hold(Key::S);
        backend->inputs[0].press(Key::A);
        app.initialize(std::move(backend),{resources,script});
        ASSERT_EQ(app.getSimulator()->getEngine()->getCylinderCount(),engine.second);
        data->onShutdown=[&] { EXPECT_EQ(app.getSimulator(),nullptr); };
        app.run(); EXPECT_EQ(data->frames.size(),40U); EXPECT_GT(data->pcm.size(),0U);
        EXPECT_TRUE(std::any_of(data->pcm.begin(),data->pcm.end(),[](std::int16_t value) { return value!=0; }));
        EXPECT_TRUE(model(data->frames.back(),"Piston"));
        app.destroy(); EXPECT_EQ(data->shutdowns,1);
    }
}
}
