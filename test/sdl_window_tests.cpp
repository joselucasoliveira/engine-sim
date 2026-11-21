#include <gtest/gtest.h>
#include "sdl_window.h"
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

namespace {
using desktop::Key;
class DesktopInput : public testing::Test {
protected:
    desktop::SdlWindow window;
    void SetUp() override {
        window.initialize({"Engine Simulator input integration",640,360,true});
        ASSERT_NE(window.nativeHandle(),nullptr);
        driver=window.videoDriver();
        ASSERT_NE(driver,"dummy"); ASSERT_NE(driver,"offscreen"); ASSERT_FALSE(driver.empty());
        std::cout<<"SDL "<<SDL_GetVersion()<<" video="<<driver<<" hidden real window; no GPU/audio qualification\n";
        window.pumpEvents(); focus(true); window.pumpEvents();
        ASSERT_TRUE(window.frame().window.active);
    }
    void TearDown() override { window.shutdown(); SDL_Quit(); }
    void push(SDL_Event event) { ASSERT_TRUE(SDL_PushEvent(&event))<<SDL_GetError(); }
    void focus(bool active) {
        SDL_Event e{}; e.type=active?SDL_EVENT_WINDOW_FOCUS_GAINED:SDL_EVENT_WINDOW_FOCUS_LOST;
        e.window.windowID=SDL_GetWindowID(window.nativeHandle()); push(e);
    }
    void key(SDL_Scancode scan,bool down,bool repeat=false,SDL_WindowID id=0) {
        SDL_Event e{}; e.type=down?SDL_EVENT_KEY_DOWN:SDL_EVENT_KEY_UP;
        e.key.windowID=id?id:SDL_GetWindowID(window.nativeHandle());
        e.key.scancode=scan; e.key.down=down; e.key.repeat=repeat; push(e);
    }
    void wheel(float amount, bool flipped=false) {
        SDL_Event e{}; e.type=SDL_EVENT_MOUSE_WHEEL; e.wheel.windowID=SDL_GetWindowID(window.nativeHandle());
        e.wheel.y=amount; e.wheel.direction=flipped?SDL_MOUSEWHEEL_FLIPPED:SDL_MOUSEWHEEL_NORMAL; push(e);
    }
    void button(bool down,float x,float y) {
        SDL_Event e{}; e.type=down?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID=SDL_GetWindowID(window.nativeHandle()); e.button.button=SDL_BUTTON_LEFT;
        e.button.down=down; e.button.x=x; e.button.y=y; push(e);
    }
    std::string driver;
};
TEST_F(DesktopInput, EntireLegacyPhysicalKeyTableProducesConsumableEdges) {
    const std::array<std::pair<Key,SDL_Scancode>,39> table{{
        {Key::Escape,SDL_SCANCODE_ESCAPE},{Key::Return,SDL_SCANCODE_RETURN},{Key::Tab,SDL_SCANCODE_TAB},
        {Key::F,SDL_SCANCODE_F},{Key::Insert,SDL_SCANCODE_INSERT},{Key::Right,SDL_SCANCODE_RIGHT},
        {Key::Space,SDL_SCANCODE_SPACE},{Key::Shift,SDL_SCANCODE_LSHIFT},{Key::Up,SDL_SCANCODE_UP},
        {Key::Down,SDL_SCANCODE_DOWN},{Key::N1,SDL_SCANCODE_1},{Key::N2,SDL_SCANCODE_2},
        {Key::N3,SDL_SCANCODE_3},{Key::N4,SDL_SCANCODE_4},{Key::N5,SDL_SCANCODE_5},
        {Key::F1,SDL_SCANCODE_F1},{Key::F2,SDL_SCANCODE_F2},{Key::F3,SDL_SCANCODE_F3},
        {Key::Z,SDL_SCANCODE_Z},{Key::X,SDL_SCANCODE_X},{Key::C,SDL_SCANCODE_C},{Key::V,SDL_SCANCODE_V},
        {Key::B,SDL_SCANCODE_B},{Key::N,SDL_SCANCODE_N},{Key::G,SDL_SCANCODE_G},{Key::Q,SDL_SCANCODE_Q},
        {Key::W,SDL_SCANCODE_W},{Key::E,SDL_SCANCODE_E},{Key::R,SDL_SCANCODE_R},{Key::M,SDL_SCANCODE_M},
        {Key::OEM_Comma,SDL_SCANCODE_COMMA},{Key::D,SDL_SCANCODE_D},{Key::H,SDL_SCANCODE_H},
        {Key::S,SDL_SCANCODE_S},{Key::A,SDL_SCANCODE_A},{Key::T,SDL_SCANCODE_T},{Key::U,SDL_SCANCODE_U},
        {Key::Y,SDL_SCANCODE_Y},{Key::I,SDL_SCANCODE_I}
    }};
    for (const auto &mapping:table) {
        SCOPED_TRACE(static_cast<int>(mapping.first));
        key(mapping.second,true); window.pumpEvents();
        EXPECT_TRUE(window.isKeyDown(mapping.first)); EXPECT_TRUE(window.processKeyDown(mapping.first));
        EXPECT_FALSE(window.processKeyDown(mapping.first));
        window.pumpEvents(); EXPECT_TRUE(window.isKeyDown(mapping.first)); EXPECT_FALSE(window.processKeyDown(mapping.first));
        key(mapping.second,false); window.pumpEvents(); EXPECT_FALSE(window.isKeyDown(mapping.first));
    }
}
TEST_F(DesktopInput, RepeatAndRapidReleasePreserveTheFirstEdgeAndFinalHeldState) {
    key(SDL_SCANCODE_A,true); key(SDL_SCANCODE_A,true,true); window.pumpEvents();
    EXPECT_TRUE(window.processKeyDown(Key::A)); EXPECT_TRUE(window.isKeyDown(Key::A));
    key(SDL_SCANCODE_A,true,true); window.pumpEvents(); EXPECT_FALSE(window.processKeyDown(Key::A));
    key(SDL_SCANCODE_A,false); key(SDL_SCANCODE_A,true); key(SDL_SCANCODE_A,false); window.pumpEvents();
    EXPECT_TRUE(window.processKeyDown(Key::A)); EXPECT_FALSE(window.isKeyDown(Key::A));
    key(SDL_SCANCODE_S,true,false,SDL_GetWindowID(window.nativeHandle())+1000); window.pumpEvents();
    EXPECT_FALSE(window.isKeyDown(Key::S)); EXPECT_FALSE(window.processKeyDown(Key::S));
}
TEST_F(DesktopInput, ShiftSidesAggregateAndFocusLossClearsAllInput) {
    key(SDL_SCANCODE_LSHIFT,true); key(SDL_SCANCODE_RSHIFT,true); window.pumpEvents();
    EXPECT_TRUE(window.processKeyDown(Key::Shift)); EXPECT_TRUE(window.isKeyDown(Key::Shift));
    key(SDL_SCANCODE_LSHIFT,false); window.pumpEvents(); EXPECT_TRUE(window.isKeyDown(Key::Shift));
    EXPECT_FALSE(window.processKeyDown(Key::Shift));
    key(SDL_SCANCODE_S,true); button(true,24,30); focus(false); window.pumpEvents();
    EXPECT_FALSE(window.frame().window.active); EXPECT_FALSE(window.isKeyDown(Key::Shift));
    EXPECT_FALSE(window.isKeyDown(Key::S)); EXPECT_FALSE(window.processKeyDown(Key::S));
    EXPECT_FALSE(window.processMouseDown(desktop::Button::Left));
    key(SDL_SCANCODE_R,true); wheel(1); window.pumpEvents();
    EXPECT_FALSE(window.isKeyDown(Key::R)); EXPECT_EQ(window.mouseWheel(),0);
    focus(true); key(SDL_SCANCODE_S,true); window.pumpEvents();
    EXPECT_TRUE(window.isKeyDown(Key::S)); EXPECT_TRUE(window.processKeyDown(Key::S));
    EXPECT_FALSE(window.isKeyDown(Key::Shift));
}
TEST_F(DesktopInput, PointerAndFractionalWheelUseLegacyLogicalCoordinatesAndUnits) {
    button(true,120.75f,40.25f); wheel(0.25f); window.pumpEvents();
    int x=0,y=0; window.mousePosition(&x,&y);
    EXPECT_EQ(x,120); EXPECT_EQ(y,320); EXPECT_EQ(window.mouseWheel(),30);
    EXPECT_TRUE(window.processMouseDown(desktop::Button::Left)); EXPECT_FALSE(window.processMouseDown(desktop::Button::Left));
    key(SDL_SCANCODE_SPACE,true); wheel(-1); button(false,150,60); window.pumpEvents();
    EXPECT_TRUE(window.isKeyDown(Key::Space)); EXPECT_EQ(window.mouseWheel(),-90);
    EXPECT_TRUE(window.processMouseUp(desktop::Button::Left)); EXPECT_FALSE(window.processMouseUp(desktop::Button::Left));
    wheel(-2,true); window.pumpEvents(); EXPECT_EQ(window.mouseWheel(),150);
    window.pumpEvents(); EXPECT_EQ(window.mouseWheel(),150);
    window.mousePosition(nullptr,nullptr);
}
TEST_F(DesktopInput, ResizeQueriesBothActualLogicalAndFramebufferExtents) {
    ASSERT_TRUE(SDL_SetWindowSize(window.nativeHandle(),900,500));
    ASSERT_TRUE(SDL_SyncWindow(window.nativeHandle()));
    SDL_Event event{}; event.type=SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    event.window.windowID=SDL_GetWindowID(window.nativeHandle());
    event.window.data1=12345; event.window.data2=12345; push(event); window.pumpEvents();
    int width=0,height=0,pixelsWidth=0,pixelsHeight=0;
    ASSERT_TRUE(SDL_GetWindowSize(window.nativeHandle(),&width,&height));
    ASSERT_TRUE(SDL_GetWindowSizeInPixels(window.nativeHandle(),&pixelsWidth,&pixelsHeight));
    EXPECT_EQ(width,900); EXPECT_EQ(height,500);
    EXPECT_EQ(window.frame().window.gameWidth,width); EXPECT_EQ(window.frame().window.gameHeight,height);
    EXPECT_EQ(window.frame().window.framebufferWidth,pixelsWidth); EXPECT_EQ(window.frame().window.framebufferHeight,pixelsHeight);
    button(true,450,125); window.pumpEvents(); int x=0,y=0; window.mousePosition(&x,&y);
    EXPECT_EQ(x,450); EXPECT_EQ(y,375);
    std::cout<<"logical="<<width<<'x'<<height<<" framebuffer="<<pixelsWidth<<'x'<<pixelsHeight<<'\n';
}
TEST_F(DesktopInput, FullscreenTimingCloseQuitAndRepeatedLifetimeRemainOwned) {
    const auto before=window.frame().monotonicNanoseconds;
    SDL_Delay(2); window.setFullscreen(true); window.pumpEvents(); EXPECT_TRUE(window.frame().window.fullscreen);
    EXPECT_GE(window.frame().monotonicNanoseconds,before); EXPECT_GT(window.frame().seconds,0);
    EXPECT_TRUE(std::isfinite(window.frame().averageFps)); EXPECT_GT(window.frame().averageFps,0);
    window.setFullscreen(false); window.pumpEvents(); EXPECT_FALSE(window.frame().window.fullscreen);
    SDL_Event close{}; close.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    close.window.windowID=SDL_GetWindowID(window.nativeHandle()); push(close); window.pumpEvents();
    EXPECT_FALSE(window.frame().window.open); window.shutdown(); window.shutdown();
    EXPECT_EQ(window.nativeHandle(),nullptr);
    ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO));
    for (int i=0;i<3;++i) {
        window.initialize({"Repeated input lifetime",640,360,true}); window.pumpEvents();
        EXPECT_EQ(window.mouseWheel(),0); EXPECT_FALSE(window.isKeyDown(Key::S));
        SDL_Event quit{}; quit.type=SDL_EVENT_QUIT; push(quit); window.pumpEvents(); EXPECT_FALSE(window.frame().window.open);
        window.shutdown(); EXPECT_NE(SDL_WasInit(SDL_INIT_VIDEO),0U);
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
TEST_F(DesktopInput, WorkerThreadCannotOperateTheVideoOwner) {
    std::atomic<bool> rejected{false};
    std::thread worker([&] { try { window.pumpEvents(); } catch (const desktop::DesktopError &) { rejected=true; } });
    worker.join(); EXPECT_TRUE(rejected); EXPECT_TRUE(window.frame().window.open);
    EXPECT_THROW(window.initialize({}),desktop::DesktopError);
}
TEST_F(DesktopInput, InvalidDimensionsAndUnavailableVideoFailWithoutRetainedResources) {
    window.shutdown(); EXPECT_THROW(window.initialize({"Invalid",0,360,true}),desktop::DesktopError);
    EXPECT_EQ(window.nativeHandle(),nullptr);
    ASSERT_TRUE(SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER,"engine-sim-unavailable",SDL_HINT_OVERRIDE));
    EXPECT_THROW(window.initialize({}),desktop::DesktopError); EXPECT_EQ(window.nativeHandle(),nullptr);
    EXPECT_EQ(SDL_WasInit(SDL_INIT_VIDEO),0U);
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER); SDL_Quit();
    EXPECT_NO_THROW(window.initialize({"Recovered",640,360,true}));
    EXPECT_EQ(window.videoDriver(),driver);
}
}
int main(int argc,char **argv) {
    testing::InitGoogleTest(&argc,argv);
    const int result=RUN_ALL_TESTS(); SDL_Quit(); return result;
}
