#include <gtest/gtest.h>
#include "engine_sim_application.h"
#include "rendering_host.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
namespace fs=std::filesystem;
using namespace desktop;
class DesktopRender : public testing::Test {
protected:
    SdlWindow window;
    SdlRenderer renderer;
    engine_sim::ResourcePaths paths;
    engine_sim::CompiledScene scene;
    fs::path scratch;
    void SetUp() override {
        const char *root=std::getenv("ENGINE_SIM_RENDER_RESOURCES");ASSERT_NE(root,nullptr);
        paths=engine_sim::ResourcePaths::resolve(engine_sim::executablePath(),{fs::u8path(root),{}},false);
        paths.validateBase();scene=engine_sim::readCompiledScene(paths.scene());ASSERT_EQ(scene.meshes.size(),9U);
        const auto cwd=fs::canonical(fs::current_path());ASSERT_NE(cwd,paths.root);
        ASSERT_TRUE(!fs::exists(cwd/"CMakeLists.txt"));
        scratch=cwd/(std::string(testing::UnitTest::GetInstance()->current_test_info()->name())+"-"
            +std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directory(scratch);
        std::srand(1);
    }
    void TearDown() override { renderer.shutdown();window.shutdown();SDL_Quit(); }
    void open() {
        window.initialize({"Engine Simulator renderer integration",640,360,false});window.pumpEvents();
        ASSERT_NE(window.videoDriver(),"dummy");ASSERT_NE(window.videoDriver(),"offscreen");
        renderer.initialize(window.nativeHandle(),paths,scene);
#ifdef __APPLE__
        ASSERT_EQ(renderer.statistics().driver,"metal");
#else
        ASSERT_EQ(renderer.statistics().driver,"vulkan");
#endif
        std::cout<<"video="<<window.videoDriver()<<" GPU="<<renderer.statistics().driver<<"; actual pixels, endpoint audio not exercised\n";
    }
    static void nearPixel(const std::vector<std::uint8_t> &pixels,int width,int x,int y,int r,int g,int b) {
        const auto offset=static_cast<std::size_t>(y*width+x)*4;ASSERT_LT(offset+3,pixels.size());
        EXPECT_NEAR(pixels[offset],r,2);EXPECT_NEAR(pixels[offset+1],g,2);EXPECT_NEAR(pixels[offset+2],b,2);
    }
    static std::vector<Vertex> quad(float radius=0.8f) {
        std::vector<Vertex> v(4);v[0].Pos={-radius,radius,0,0};v[1].Pos={radius,radius,0,0};
        v[2].Pos={radius,-radius,0,0};v[3].Pos={-radius,-radius,0,0};return v;
    }
};
TEST_F(DesktopRender, GammaAlphaStageLayerViewportWindingResizeAndReadback) {
    open();host_test::RecordingHost host(std::make_shared<host_test::Recording>());host.loadModels(scene);
    const auto v=quad();std::uint16_t i[]{2,1,0,3,2,0};renderer.uploadGeometry(v.data(),v.size(),i,6);
    host.drawing.viewport={0,0,640,360};host.drawing.clearColor={0,0,0.1f,1};host.drawing.color={0.5f,0,0,1};
    host.drawGenerated(SceneStage,0,0,2,0);renderer.render(host,window.frame());auto pixels=renderer.readPixels();
    nearPixel(pixels,640,320,180,188,0,0);nearPixel(pixels,640,5,5,0,0,26);
    host.clearPackets();host.drawing.color={0.5f,0,0,0.5f};host.drawGenerated(SceneStage,0,0,2,0);
    renderer.render(host,window.frame());pixels=renderer.readPixels();nearPixel(pixels,640,320,180,94,0,13);
    host.clearPackets();host.drawing.color={0,0.5f,0,1};host.drawGenerated(UiStage,0,0,2,0);
    host.drawing.color={0.5f,0,0,1};host.drawGenerated(SceneStage,0,0,2,255);
    renderer.render(host,window.frame());pixels=renderer.readPixels();nearPixel(pixels,640,320,180,0,188,0);
    host.clearPackets();host.drawing.color={0,0.5f,0,1};host.drawGenerated(SceneStage,0,0,2,20);
    host.drawing.color={0.5f,0,0,1};host.drawGenerated(SceneStage,0,0,2,0);
    renderer.render(host,window.frame());pixels=renderer.readPixels();nearPixel(pixels,640,320,180,0,188,0);
    host.clearPackets();host.drawing.viewport={320,0,320,360};host.drawing.color={0.5f,0,0,1};host.drawGenerated(SceneStage,0,0,2,0);
    renderer.render(host,window.frame());pixels=renderer.readPixels();nearPixel(pixels,640,480,180,188,0,0);nearPixel(pixels,640,160,180,0,0,26);
    host.clearPackets();host.drawing.viewport={0,0,640,360};host.drawing.transform=math::TranslationTransform({0.5f,0,0,0});
    host.drawGenerated(SceneStage,0,0,2,0);const auto small=quad(0.1f);renderer.uploadGeometry(small.data(),small.size(),i,6);
    renderer.render(host,window.frame());pixels=renderer.readPixels();nearPixel(pixels,640,480,180,188,0,0);nearPixel(pixels,640,320,180,0,0,26);
    std::uint16_t back[]{0,1,2,0,2,3};renderer.uploadGeometry(v.data(),v.size(),back,6);
    host.clearPackets();host.drawing.transform=math::LoadIdentity();host.drawGenerated(SceneStage,0,0,2,0);
    renderer.render(host,window.frame());pixels=renderer.readPixels();nearPixel(pixels,640,320,180,0,0,26);
    ASSERT_TRUE(SDL_SetWindowSize(window.nativeHandle(),900,500));ASSERT_TRUE(SDL_SyncWindow(window.nativeHandle()));window.pumpEvents();
    host.clearPackets();host.drawing.viewport={0,0,900,500};host.drawGenerated(SceneStage,0,0,2,0);renderer.uploadGeometry(v.data(),v.size(),i,6);
    renderer.render(host,window.frame());pixels=renderer.readPixels();ASSERT_EQ(pixels.size(),900U*500*4);
    nearPixel(pixels,900,450,250,188,0,0);nearPixel(pixels,900,5,5,0,0,26);
    render_test::ppm(scratch/"analytic.ppm",pixels,900,500);
    EXPECT_EQ(renderer.statistics().submittedFrames,8U);EXPECT_EQ(renderer.statistics().presentedFrames,8U);
}
TEST_F(DesktopRender, OriginalFontMetricsAndGlyphTextureProduceVisibleLabels) {
    open();FontAtlas font;font.load(paths.font());
    EXPECT_FLOAT_EQ(font.glyph('A').advance,45.71428680419922f);EXPECT_FLOAT_EQ(font.glyph('I').advance,22.85714340209961f);
    EXPECT_FLOAT_EQ(font.glyph('W').advance,53.33333206176758f);EXPECT_FLOAT_EQ(font.glyph(' ').advance,30.4761905670166f);
    EXPECT_FLOAT_EQ(font.glyph('A').p0.x,7);EXPECT_FLOAT_EQ(font.glyph('A').p0.y,-39);EXPECT_FLOAT_EQ(font.glyph('A').p1.x,39);
    EXPECT_FLOAT_EQ(renderer.measureText("AIW",64),font.glyph('A').advance+font.glyph('I').advance+font.glyph('W').advance);
    EXPECT_THROW(font.glyph(1),DesktopError);
    host_test::RecordingHost host(std::make_shared<host_test::Recording>());
    host.drawing.clearColor={0,0,0,1};host.drawing.ui.projection=math::Transpose(math::OrthographicProjection(640,360,0.001f,500));
    host.drawing.ui.view=math::Transpose(math::CameraTarget({0,0,10,1},{0,0,0,1},math::Constants::YAxis));
    host.drawText("ENGINE SIMULATOR\nRPM 1234",-280,80,40,{1,1,1,1});renderer.render(host,window.frame());const auto pixels=renderer.readPixels();
    std::size_t foreground=0;for(std::size_t k=0;k<pixels.size();k+=4)if(pixels[k]>32)++foreground;
    EXPECT_GT(foreground,1000U);EXPECT_LT(foreground,10000U);EXPECT_EQ(renderer.statistics().glyphs,24U);
    render_test::ppm(scratch/"labels.ppm",pixels,640,360);
}
TEST_F(DesktopRender, ActualApplicationRendersAllScreensAndNineCylinderEngine) {
    for (bool radial:{false,true}) {
        EngineSimApplication app;auto data=std::make_shared<render_test::Output>();const auto directory=scratch/(radial?"radial":"default");fs::create_directory(directory);
        auto host=std::make_unique<render_test::RenderingHost>(data,directory,radial?40:4);host->tabs=!radial;host->startEngine=radial;
        auto options=engine_sim::ResourceOptions{paths.root,{}};
        if(radial){options.script=directory/"radial.mr";std::ofstream s(options.script);s<<"import \"engine_sim.mr\"\nimport \"engines/atg-video-2/09_radial_9.mr\"\nmain()\n";}
        app.initialize(std::move(host),options);ASSERT_EQ(app.getSimulator()->getEngine()->getCylinderCount(),radial?9:4);
        data->onShutdown=[&]{EXPECT_EQ(app.getSimulator(),nullptr);};app.run();app.destroy();
        EXPECT_EQ(data->shutdowns,1);EXPECT_EQ(data->gpu.submittedFrames,radial?40U:4U);EXPECT_EQ(data->gpu.presentedFrames,radial?40U:4U);
        EXPECT_GT(data->gpu.draws,100U);EXPECT_GT(data->gpu.glyphs,100U);
        EXPECT_EQ(data->gpu.width,radial?2560:1920);EXPECT_EQ(data->gpu.height,radial?1440:1080);
        for(const char *name:{"Crankshaft","CrankSnout","CrankSnoutThreads","Piston","ConnectingRod","CylinderHead","Valve"})EXPECT_TRUE(data->models.count(name))<<name;
        if(!radial){EXPECT_TRUE(data->models.count("Logo"));EXPECT_TRUE(data->labels.count("ENGINE SIMULATOR"));ASSERT_EQ(data->sceneViews.size(),4U);EXPECT_LT(data->sceneViews[0].width,data->sceneViews[1].width);EXPECT_LT(data->sceneViews[3].width,data->sceneViews[1].width);}
        else EXPECT_TRUE(std::any_of(data->audio->pcm.begin(),data->audio->pcm.end(),[](auto sample){return sample!=0;}));
    }
}
TEST_F(DesktopRender, VisibleLoadDiagnosticRetainsRunningRadialAndRecoversOnNativeReturn) {
    EngineSimApplication app;auto data=std::make_shared<render_test::Output>();
    const auto entry=scratch/fs::u8path("entry Ω ç.mr");
    const std::string source="import \"engine_sim.mr\"\nimport \"engines/atg-video-2/09_radial_9.mr\"\nmain()\n";
    std::ofstream(entry)<<source;
    auto host=std::make_unique<render_test::RenderingHost>(data,scratch,24);host->startEngine=true;
    const Simulator *original=nullptr;bool rejected=false,recovered=false;
    host->beforeEvents=[&](std::size_t frame,SDL_Window *window) {
        if (frame==4) std::ofstream(entry)<<"invalid !!!\n";
        if (frame==9) std::ofstream(entry)<<source;
        if (frame==4 || frame==5 || frame==9 || frame==10) {
            const bool down=frame==4 || frame==9;SDL_Event event{};
            event.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;event.key.windowID=SDL_GetWindowID(window);
            event.key.scancode=SDL_SCANCODE_RETURN;event.key.down=down;ASSERT_TRUE(SDL_PushEvent(&event));
        }
        if (frame==5) {
            rejected=true;EXPECT_EQ(app.getSimulator(),original);
            const auto raw=app.getInfoCluster()->getLogMessage();
            EXPECT_NE(raw.find("Ω ç"),std::string::npos);EXPECT_NE(raw.find('\n'),std::string::npos);
            EXPECT_GT(std::abs(original->getEngine()->getRpm()),0);
        }
        if (frame==10) {
            recovered=true;EXPECT_NE(app.getSimulator(),original);
            EXPECT_EQ(app.getSimulator()->getEngine()->getCylinderCount(),9);
        }
    };
    app.initialize(std::move(host),{paths.root,entry});original=app.getSimulator();
    ASSERT_NE(original,nullptr);ASSERT_EQ(original->getEngine()->getCylinderCount(),9);
    data->onShutdown=[&]{EXPECT_EQ(app.getSimulator(),nullptr);};
    app.run();app.destroy();EXPECT_TRUE(rejected);EXPECT_TRUE(recovered);EXPECT_EQ(data->shutdowns,1);
    EXPECT_EQ(data->gpu.presentedFrames,24U);EXPECT_GT(data->gpu.glyphs,100U);
    bool diagnostic=false;
    for (const auto &label:data->labels) {
        if (label.find("entry ? ?.mr")!=std::string::npos) diagnostic=true;
        for (unsigned char byte:label) EXPECT_TRUE(byte=='\n' || (byte>=32 && byte<=127));
    }
    EXPECT_TRUE(diagnostic);
}
TEST_F(DesktopRender, MissingMalformedInputsAndRepeatedGpuLifetimeFailOrReleaseCleanly) {
    window.initialize({"GPU failure lifecycle",640,360,false});window.pumpEvents();
    auto absent=paths;absent.desktop=scratch/"absent";EXPECT_THROW(renderer.initialize(window.nativeHandle(),absent,scene),engine_sim::ResourceError);
    auto invalidScene=scene;invalidScene.meshes[0].indices[0]=65535;
    EXPECT_THROW(renderer.initialize(window.nativeHandle(),paths,invalidScene),DesktopError);
    const auto bad=scratch/"bad";fs::create_directories(bad/"fonts/Silkscreen");fs::create_directories(bad/"shaders/sdl");
    fs::copy_file(paths.font(),bad/"fonts/Silkscreen/slkscr.ttf");
    std::ofstream(bad/"shaders/sdl/engine_sim.vertex.spv")<<"invalid module";
    std::ofstream(bad/"shaders/sdl/engine_sim.vertex.msl")<<"invalid module";
    absent.desktop=bad;EXPECT_THROW(renderer.initialize(window.nativeHandle(),absent,scene),engine_sim::ResourceError);
    for(int iteration=0;iteration<3;++iteration){renderer.initialize(window.nativeHandle(),paths,scene);EXPECT_THROW(renderer.initialize(window.nativeHandle(),paths,scene),DesktopError);renderer.shutdown();renderer.shutdown();}
    renderer.initialize(window.nativeHandle(),paths,scene);EXPECT_THROW(renderer.readPixels(),DesktopError);
}
}
int main(int argc,char **argv){testing::InitGoogleTest(&argc,argv);const int result=RUN_ALL_TESTS();SDL_Quit();return result;}
