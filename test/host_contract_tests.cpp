#include <gtest/gtest.h>
#include "recording_host.h"
#include <cmath>
#include <limits>

namespace {
using namespace desktop;
using namespace desktop::math;
TEST(HostContract, MatricesPreserveCoordinatesCameraAndVertexLayout) {
    const auto transform=MatMult(TranslationTransform({3,-2,4,0}),ScaleTransform({2,3,4,0}));
    const auto point=MatMult(transform,Vector{1,2,3,1});
    EXPECT_FLOAT_EQ(point.x,5); EXPECT_FLOAT_EQ(point.y,4); EXPECT_FLOAT_EQ(point.z,16); EXPECT_FLOAT_EQ(point.w,1);
    const auto rotated=MatMult(RotationTransform(Constants::ZAxis,Constants::PI/2),Vector{1,0,0,1});
    EXPECT_NEAR(rotated.x,0,1e-6); EXPECT_NEAR(rotated.y,1,1e-6); EXPECT_FLOAT_EQ(rotated.w,1);
    const auto view=CameraTarget({0,0,10,1},{0,0,0,1},Constants::YAxis);
    const auto eye=MatMult(Transpose(view),Vector{0,0,10,1});
    EXPECT_NEAR(eye.x,0,1e-6); EXPECT_NEAR(eye.y,0,1e-6); EXPECT_NEAR(eye.z,0,1e-6);
    const auto projection=OrthographicProjection(1920,1080,0.001f,500);
    EXPECT_FLOAT_EQ(projection.rows[0].x,2.0f/1920); EXPECT_FLOAT_EQ(projection.rows[1].y,2.0f/1080);
    EXPECT_FLOAT_EQ(projection.rows[2].z,2.0f/(500-0.001f)); EXPECT_FLOAT_EQ(projection.rows[3].w,1);
    EXPECT_EQ(sizeof(Vertex),40U); EXPECT_EQ(offsetof(Vertex,TexCoord),16U); EXPECT_EQ(offsetof(Vertex,Normal),24U);
}
TEST(HostContract, PacketsOwnTransformColorStageLayerAndText) {
    auto recording=std::make_shared<host_test::Recording>(); host_test::RecordingHost host(recording);
    engine_sim::CompiledScene scene; scene.meshes.resize(1); scene.meshes[0].name="analytic triangle"; scene.meshes[0].indices={0,1,2};
    host.loadModels(scene); host.drawing.color={1,0,0,1}; host.drawing.transform=TranslationTransform({4,5,0,0});
    host.DrawModel(SceneStage,host.GetModelAsset("analytic triangle"),17);
    host.drawing.color={0,1,0,1}; host.drawGenerated(UiStage,0,0,1,3);
    host.drawText("first\nsecond",10,20,12,{0,0,1,1}); host.drawing.transform=LoadIdentity();
    const auto &packets=host.drawPackets(); ASSERT_EQ(packets.size(),3U);
    EXPECT_EQ(packets[0].stage,SceneStage); EXPECT_EQ(packets[0].layer,17); EXPECT_EQ(packets[0].model,"analytic triangle");
    EXPECT_FLOAT_EQ(packets[0].state.color.x,1); EXPECT_FLOAT_EQ(packets[0].state.transform.rows[0].w,4);
    EXPECT_EQ(packets[1].stage,UiStage); EXPECT_FLOAT_EQ(packets[1].state.color.y,1);
    EXPECT_EQ(packets[2].stage,TextStage); EXPECT_EQ(packets[2].text,"first\nsecond"); EXPECT_FLOAT_EQ(packets[2].state.color.z,1);
    EXPECT_THROW(host.drawGenerated(UiStage,0,0,1,256),DesktopError);
    host.clearPackets(); EXPECT_TRUE(host.drawPackets().empty());
}
TEST(HostContract, GeometryRejectsNonfiniteValuesInvalidIndicesAndRanges) {
    auto recording=std::make_shared<host_test::Recording>(); host_test::RecordingHost host(recording);
    std::array<Vertex,3> vertices{}; std::array<std::uint16_t,3> indices{0,1,2};
    host.drawGenerated(SceneStage,0,0,1,0);
    EXPECT_NO_THROW(host.validateGeometry(vertices.data(),3,indices.data(),3));
    indices[2]=3; EXPECT_THROW(host.validateGeometry(vertices.data(),3,indices.data(),3),DesktopError); indices[2]=2;
    vertices[1].Pos.x=std::numeric_limits<float>::quiet_NaN();
    EXPECT_THROW(host.validateGeometry(vertices.data(),3,indices.data(),3),DesktopError); vertices[1].Pos.x=0;
    EXPECT_THROW(host.validateGeometry(vertices.data(),100001,indices.data(),3),DesktopError);
    EXPECT_THROW(host.validateGeometry(nullptr,3,indices.data(),3),DesktopError);
    EXPECT_THROW(host.validateGeometry(vertices.data(),3,indices.data(),2),DesktopError);
    host.clearPackets(); host.drawGenerated(SceneStage,-1,0,1,0);
    EXPECT_THROW(host.validateGeometry(vertices.data(),3,indices.data(),3),DesktopError);
}
TEST(HostContract, ModelsAndWindowKeepLogicalAndFramebufferIdentity) {
    auto recording=std::make_shared<host_test::Recording>(); host_test::RecordingHost host(recording);
    engine_sim::CompiledScene scene; scene.meshes.resize(1); scene.meshes[0].name="analytic"; scene.meshes[0].indices={0,1,2};
    host.loadModels(scene); auto *model=host.GetModelAsset("analytic");
    host.state.window={1280,720,1280,720,2560,1440,true,true,false};
    EXPECT_EQ(host.GetScreenWidth(),1280); EXPECT_EQ(host.GetGameWindow()->GetGameHeight(),720);
    EXPECT_EQ(host.frame().window.framebufferWidth,2560); EXPECT_EQ(host.GetModelAsset("analytic"),model);
    host.GetGameWindow()->SetWindowStyle(WindowStyle::Fullscreen);
    EXPECT_EQ(host.GetGameWindow()->GetWindowStyle(),WindowStyle::Fullscreen); EXPECT_TRUE(host.GetGameWindow()->IsActive());
    EXPECT_THROW(host.GetModelAsset("absent"),DesktopError);
    EXPECT_THROW(host.GetModelAsset(nullptr),DesktopError);
    EXPECT_THROW(host.readScreen(nullptr,0),DesktopError);
}
}
