#include "runtime.hpp"
#include "shaft_drive.hpp"
#include "checked_solver.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
#include <future>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace engine_sim {
struct RuntimeTestAccess {
    static Simulator &simulator(es_runtime &r) {return *r.implementation.m_session->prepared.session->simulator;}
    static PullEngine &pull(es_runtime &r) {return *r.implementation.m_session->pull;}
};
}
namespace {
using namespace engine_sim;
namespace fs=std::filesystem;
using Handle=std::unique_ptr<es_runtime,decltype(&es_destroy)>;
void checked(es_status s,const es_error &e) {if(s!=ES_OK)throw std::runtime_error(e.message);}
es_telemetry telemetry(es_runtime *r) {es_telemetry t{};t.size=sizeof(t);t.abi=ES_ABI_V2;es_error e{};checked(es_get_telemetry(r,&t,&e),e);return t;}
void controls(es_runtime *r,double rpm,double load=0,bool ignition=true) {es_error e{};es_controls c{sizeof(c),ES_ABI_V2,static_cast<std::uint32_t>(ignition),0,.2,load,rpm};checked(es_set_controls(r,&c,&e),e);}
std::vector<float> render(es_runtime *r,std::size_t count,std::size_t part=257) {
    es_error e{};std::vector<float> out(count);
    for(std::size_t n=0;n<count;) {const auto take=static_cast<std::uint32_t>(std::min(part,count-n));std::uint32_t written=99;checked(es_render(r,take,out.data()+n,take,&written,&e),e);if(written!=take)throw std::runtime_error("short successful render");n+=take;}return out;
}
// Inspect the existing model's wiring without mutating or replacing ignition.
struct IgnitionAccess:IgnitionModule {
    static auto plugs() {return &IgnitionAccess::m_plugs;}
};
double joints(Engine &engine) {
    double worst=0;
    for(int i=0;i<engine.getCylinderCount();++i) {
        auto &piston=*engine.getPiston(i);auto &rod=*piston.getRod();auto &bank=*piston.getCylinderBank();
        double bx,by,sx,sy,wx,wy,jx,jy;rod.m_body.localToWorld(0,rod.getBigEndLocal(),&bx,&by);rod.m_body.localToWorld(0,rod.getLittleEndLocal(),&sx,&sy);piston.m_body.localToWorld(0,piston.getWristPinLocation(),&wx,&wy);
        if(rod.getMasterRod())rod.getMasterRod()->getRodJournalPositionGlobal(rod.getJournal(),&jx,&jy);
        else {auto &crank=*rod.getCrankshaft();const double a=crank.getRodJournalAngle(rod.getJournal())+crank.m_body.theta;jx=crank.m_body.p_x+crank.getThrow()*std::cos(a);jy=crank.m_body.p_y+crank.getThrow()*std::sin(a);}
        const double a=bank.getAngle()+constants::pi/2,wall=std::abs((wx-bank.getX())*std::sin(a)-(wy-bank.getY())*std::cos(a));
        worst=std::max(worst,std::max({std::hypot(bx-jx,by-jy),std::hypot(sx-wx,sy-wy),wall})/rod.getLength());
    }return worst;
}
double trajectory(double t) {return t<.05?0:t<.2?600:t<.45?600+4800*(t-.2):t<.6?1800:t<.7?1200:0;}
class Drive:public testing::Test {
protected:
    fs::path source,fixture;
    void SetUp() override {const auto *s=std::getenv("ENGINE_SIM_TEST_SOURCE");ASSERT_NE(s,nullptr);source=fs::u8path(s);fixture=source/"examples/runtime/fixture";}
    Handle prepare(const std::string &model="fixture",std::uint32_t rate=44100,std::uint32_t frequency=0) {
        fs::path assets = model == "fixture" ? fixture : source;
        fs::path entry = assets / "main.json";
        if (model == "EJ25") entry = assets / "assets/main.json";
        else if (model == "KOHLER") entry = assets / "assets/engines/atg-video-1/02_kohler_ch750.json";
        else if (model == "LS") entry = assets / "assets/engines/atg-video-2/07_gm_ls.json";
        es_error e{};es_config c{};checked(es_config_init(ES_ABI_V2,sizeof(c),&c,&e),e);c.mode=ES_DRIVEN;c.output_hz=rate;c.simulation_hz=frequency;
        es_runtime *raw=nullptr;checked(es_create(&c,&raw,&e),e);Handle owned(raw,es_destroy);checked(es_load_json_file(raw,entry.u8string().c_str(),assets.u8string().c_str(),nullptr,&e),e);return owned;
    }
};
struct Statistics {double peakJoint=0,peakError=0,peakPhase=0,peakReaction=0,plateauReaction=0,energy=0;unsigned lit=0;std::uint64_t ticks=0;};
TEST_F(Drive, RealMechanismsFollowTheTrajectoryWithJointPhaseCamAndFiringChecks) {
    const std::array<std::pair<const char *,double>,5> cases{{{"fixture",0},{"KOHLER",0},{"KOHLER",100},{"LS",100},{"EJ25",100}}};
    double kohlerFree=0,kohlerLoaded=0;
    for(const auto &item:cases) {
        SCOPED_TRACE(item.first);
        SCOPED_TRACE(item.second);auto r=prepare(item.first);auto &sim=RuntimeTestAccess::simulator(*r);auto &engine=*sim.getEngine();auto &shaft=*engine.getOutputCrankshaft();
        const auto frequency=sim.getSimulationFrequency();Statistics stats;double before=shaft.m_body.theta,previousCycle=shaft.getCycleAngle();
        RuntimeTestAccess::pull(*r).setTickObserver([&](const Simulator &){
            const auto eventFrame=stats.ticks*44100/frequency;const double target=trajectory(eventFrame/44100.);
            const double speed=shaft.m_body.v_theta,phase=std::abs(std::remainder(shaft.m_body.theta-before+units::rpm(target)/frequency,4*constants::pi));before=shaft.m_body.theta;
            stats.peakError=std::max(stats.peakError,std::abs(units::toRpm(speed)+target));stats.peakPhase=std::max(stats.peakPhase,phase);stats.peakJoint=std::max(stats.peakJoint,joints(engine));
            EXPECT_LE(std::abs(units::toRpm(speed)+target),.1);EXPECT_LE(phase,units::rpm(.1)/frequency+1e-10);
            const double cycle=shaft.getCycleAngle();auto &im=*engine.getIgnitionModule();const auto *plugs=im.*IgnitionAccess::plugs();
            for(int i=0;i<engine.getCylinderCount();++i) if(engine.getChamber(i)->popLitLastFrame()) {
                ++stats.lit;double a=std::fmod(plugs[i].angle-im.getTimingAdvance(),4*constants::pi);if(a<0)a+=4*constants::pi;double end=cycle;if(end<previousCycle){end+=4*constants::pi;a+=4*constants::pi;}
                EXPECT_TRUE(plugs[i].enabled);EXPECT_GE(a,previousCycle);EXPECT_LT(a,end);
            }
            previousCycle=cycle;
            for(int i=0;i<engine.getCylinderBankCount();++i)for(auto *cam:{engine.getHead(i)->getIntakeCamshaft(),engine.getHead(i)->getExhaustCamshaft()}) {
                double expected=std::fmod((shaft.getAngle()+cam->getAdvance())/2,2*constants::pi);if(expected<0)expected+=2*constants::pi;EXPECT_NEAR(cam->getAngle(),expected,1e-12);
            }
            ++stats.ticks;
        });
        for(std::uint32_t frame=0;frame<39690;++frame) {
            controls(r.get(),trajectory(frame/44100.),item.second);const auto pcm=render(r.get(),1);stats.energy+=pcm[0]*pcm[0];const auto t=telemetry(r.get());
            EXPECT_EQ(t.frames,frame+1u);EXPECT_EQ(t.ticks,std::uint64_t(frame+1)*frequency/44100);EXPECT_LE(t.crank_rad_s,units::rpm(.1));EXPECT_TRUE(std::isfinite(pcm[0]));
            EXPECT_EQ(t.load_reaction_nm,t.applied_driven_rpm>0?item.second:0);stats.peakReaction=std::max(stats.peakReaction,std::abs(t.drive_reaction_nm));if(frame==24695)stats.plateauReaction=t.drive_reaction_nm;
        }
        EXPECT_GT(stats.lit,0u);EXPECT_GT(stats.energy,1e-6);EXPECT_LT(stats.peakReaction,1000000);EXPECT_EQ(stats.ticks,static_cast<std::uint64_t>(frequency*.9));EXPECT_NEAR(telemetry(r.get()).measured_rpm,0,.1);
        // Independently frozen same-model mechanism residuals, before extraction.
        const double reference=std::string(item.first)=="KOHLER"?(item.second?2.2084514771942555e-5:2.0866415216569118e-5):std::string(item.first)=="LS"?.0014188754612185753:std::string(item.first)=="EJ25"?3.36219720156724e-5:.0015;
        EXPECT_LE(stats.peakJoint,2*reference+1e-7);
        if(std::string(item.first)=="KOHLER") {if(item.second)kohlerLoaded=stats.plateauReaction;else kohlerFree=stats.plateauReaction;}
        EXPECT_FALSE(sim.m_starterMotor.m_enabled);EXPECT_FALSE(sim.m_dyno.m_enabled);EXPECT_EQ(sim.getTransmission()->getGear(),-1);EXPECT_EQ(sim.getTransmission()->getClutchPressure(),0);
        std::cout<<"model="<<item.first<<" load_nm="<<item.second<<" ticks="<<stats.ticks<<" max_rpm_error="<<stats.peakError<<" max_phase_rad="<<stats.peakPhase<<" max_joint_fraction="<<stats.peakJoint<<" max_drive_nm="<<stats.peakReaction<<" plateau_drive_nm="<<stats.plateauReaction<<" lit="<<stats.lit<<" pcm_energy="<<stats.energy<<'\n';
    }
    EXPECT_GT(kohlerLoaded-kohlerFree,-110);EXPECT_LT(kohlerLoaded-kohlerFree,-90);
}
TEST_F(Drive, GradualCommandsReachTheValidatedUpperEndpointAndHoldRestAgainstGas) {
    for(const auto &model:{"fixture","KOHLER","LS","EJ25"}) {
        SCOPED_TRACE(model);auto r=prepare(model,48000);const double limit=telemetry(r.get()).driven_limit_rpm;EXPECT_GT(limit,1800);EXPECT_LE(limit,12000);
        double worst=0,peak=0;auto &sim=RuntimeTestAccess::simulator(*r);RuntimeTestAccess::pull(*r).setTickObserver([&](const Simulator &){worst=std::max(worst,joints(*sim.getEngine()));});
        for(unsigned block=0;block<200;++block) {
            const double t=block*.01,target=t<.05?0:t<.2?600:t<1.2?600+(limit-600)*(t-.2):t<1.8?limit:t<1.95?limit*(1.95-t)/.15:0;
            controls(r.get(),target,100);render(r.get(),480);const auto s=telemetry(r.get());EXPECT_NEAR(s.measured_rpm,target,.1);peak=std::max(peak,std::abs(s.drive_reaction_nm));
        }
        EXPECT_LT(peak,1000000);EXPECT_EQ(telemetry(r.get()).applied_driven_rpm,0);EXPECT_EQ(telemetry(r.get()).load_reaction_nm,0);
        const auto before=telemetry(r.get());render(r.get(),4800);EXPECT_NEAR(std::remainder(telemetry(r.get()).cycle_rad-before.cycle_rad,4*constants::pi),0,1e-9);
        const double reference=std::string(model)=="KOHLER"?3.2000520716902125e-5:std::string(model)=="LS"?.0013075680077435559:std::string(model)=="EJ25"?.00020166867612264033:.0015;
        EXPECT_LE(worst,2*reference+1e-7);std::cout<<"upper model="<<model<<" limit_rpm="<<limit<<" max_joint_fraction="<<worst<<" max_drive_nm="<<peak<<'\n';
    }
}
TEST_F(Drive, ModeBoundsAndInvalidVectorsPreservePendingControlsAtomically) {
    auto a=prepare(),b=prepare();controls(a.get(),600,10);controls(b.get(),600,10);es_error e{};const auto initial=telemetry(a.get());EXPECT_EQ(initial.applied_driven_rpm,0);EXPECT_EQ(initial.requested_load_nm,10);
    for(double rpm:{-1.,6000.001,12001.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {es_controls c{sizeof(c),ES_ABI_V2,0,0,.9,20,rpm};EXPECT_EQ(es_set_controls(a.get(),&c,&e),ES_INVALID);EXPECT_EQ(telemetry(a.get()).requested_load_nm,10);}
    es_controls c{sizeof(c),ES_ABI_V2,0,1,.9,20,600};EXPECT_EQ(es_set_controls(a.get(),&c,&e),ES_UNSUPPORTED);EXPECT_EQ(render(a.get(),4410),render(b.get(),4410));
    EXPECT_NEAR(telemetry(a.get()).measured_rpm,600,.1);EXPECT_EQ(telemetry(a.get()).mode,ES_DRIVEN);EXPECT_NE(es_capabilities()&ES_CAP_DRIVEN,0u);
}
TEST_F(Drive, SaturatedKohlerStepReturnsOnlyTheGoodPrefixAndResetRecovers) {
    auto r=prepare("KOHLER"),reference=prepare("KOHLER");controls(r.get(),0,100);controls(reference.get(),0,100);EXPECT_EQ(render(r.get(),2205),render(reference.get(),2205));
    const auto before=telemetry(r.get());const auto good=render(reference.get(),1);controls(r.get(),3600,100);std::array<float,66> pcm;pcm.fill(42);std::uint32_t written=99;es_error e{};
    EXPECT_EQ(es_render(r.get(),64,pcm.data()+1,64,&written,&e),ES_RENDER_FAULT)<<e.message;EXPECT_NE(std::strstr(e.message,"saturat"),nullptr);EXPECT_EQ(written,1u);EXPECT_EQ(pcm[1],good[0]);EXPECT_EQ(pcm[0],42);EXPECT_TRUE(std::all_of(pcm.begin()+2,pcm.end(),[](auto x){return x==42;}));
    const auto fault=telemetry(r.get());EXPECT_EQ(fault.frames,before.frames+1);EXPECT_EQ(fault.ticks,before.ticks);EXPECT_EQ(fault.state,ES_FAULTED);EXPECT_EQ(fault.applied_driven_rpm,0);
    EXPECT_EQ(es_render(r.get(),0,nullptr,0,&written,&e),ES_STATE);checked(es_reset(r.get(),&e),e);render(r.get(),2205);controls(r.get(),600,100);render(r.get(),4410);EXPECT_NEAR(telemetry(r.get()).measured_rpm,600,.1);
}
TEST_F(Drive, NonconvergenceAndCorruptedGeometryFaultBeforePublishingTheInterval) {
    for(unsigned kind=0;kind<3;++kind) {
        CheckedSolver fail;fail.m_maxIterations=0;auto r=prepare("fixture",44100,44100);auto &sim=RuntimeTestAccess::simulator(*r);controls(r.get(),100);es_error e{};
        if(kind==0)dynamic_cast<atg_scs::OptimizedNsvRigidBodySystem &>(*sim.getSystem()).initialize(&fail);
        else {unsigned ticks=0;RuntimeTestAccess::pull(*r).setTickObserver([&,kind](const Simulator &){if(++ticks==7) {if(kind==1)sim.getEngine()->getPiston(0)->m_body.p_x+=.1;else sim.getEngine()->getOutputCrankshaft()->m_body.theta=std::numeric_limits<double>::quiet_NaN();}});
            // Observer needs a counter whose lifetime spans render.
            std::array<float,32> pcm;pcm.fill(42);std::uint32_t written=99;EXPECT_EQ(es_render(r.get(),32,pcm.data(),32,&written,&e),ES_RENDER_FAULT);EXPECT_EQ(written,6u);EXPECT_NE(std::strstr(e.message,kind==1?"joint":"phase"),nullptr);EXPECT_TRUE(std::all_of(pcm.begin()+6,pcm.end(),[](auto v){return v==42;}));EXPECT_EQ(telemetry(r.get()).ticks,6u);continue;}
        std::array<float,8> pcm;pcm.fill(42);std::uint32_t written=99;EXPECT_EQ(es_render(r.get(),8,pcm.data(),8,&written,&e),ES_RENDER_FAULT);EXPECT_EQ(written,0u);EXPECT_NE(std::strstr(e.message,"converge"),nullptr);EXPECT_TRUE(std::all_of(pcm.begin(),pcm.end(),[](auto v){return v==42;}));
    }
}
TEST_F(Drive, SameControlTimelineAtBothRatesIsBitIdenticalAcrossPartitionsAndReset) {
    for(auto rate:{44100u,48000u}) {
        auto a=prepare("fixture",rate),b=prepare("fixture",rate);
        const auto run=[&](es_runtime *r,const std::vector<std::size_t> &parts) {std::vector<float> pcm;const std::array<std::size_t,9> edges{0,1,147,160,257,441,47999,48007,96003};std::size_t index=0;
            for(std::size_t i=0;i+1<edges.size();++i) {controls(r,i==0?0:i<5?600:i<7?1200:0,i%2?100:0);for(std::size_t n=edges[i];n<edges[i+1];) {const auto count=std::min(parts[index++%parts.size()],edges[i+1]-n);auto x=render(r,count);pcm.insert(pcm.end(),x.begin(),x.end());n+=count;}}return pcm;};
        const auto expected=run(a.get(),{65536});const auto ta=telemetry(a.get());EXPECT_EQ(run(b.get(),{1}),expected);const auto tb=telemetry(b.get());EXPECT_EQ(std::memcmp(&ta,&tb,sizeof(ta)),0);es_error e{};checked(es_reset(b.get(),&e),e);EXPECT_EQ(telemetry(b.get()).applied_driven_rpm,0);EXPECT_EQ(run(b.get(),{7,251,31,1,503}),expected);
    }
}
TEST_F(Drive, IndependentDrivenOwnersOverlapAndMatchIsolatedRendering) {
    auto a=prepare(),b=prepare("fixture",48000),isolatedA=prepare(),isolatedB=prepare("fixture",48000);
    const auto run=[](es_runtime *r,std::uint32_t rate) {
        controls(r,0);auto result=render(r,rate/20);controls(r,600,100);auto pcm=render(r,rate/5);result.insert(result.end(),pcm.begin(),pcm.end());return result;
    };
    const auto expectedA=run(isolatedA.get(),44100),expectedB=run(isolatedB.get(),48000);
    std::mutex mutex;std::condition_variable ready;unsigned arrived=0;bool firstA=true,firstB=true;
    const auto rendezvous=[&](bool &first) {
        if(!first)return;first=false;std::unique_lock<std::mutex> lock(mutex);++arrived;ready.notify_all();EXPECT_TRUE(ready.wait_for(lock,std::chrono::seconds(5),[&]{return arrived==2;}));
    };
    RuntimeTestAccess::pull(*a).setTickObserver([&](const Simulator &){rendezvous(firstA);});
    RuntimeTestAccess::pull(*b).setTickObserver([&](const Simulator &){rendezvous(firstB);});
    auto fa=std::async(std::launch::async,[&]{return run(a.get(),44100);});
    auto fb=std::async(std::launch::async,[&]{return run(b.get(),48000);});
    EXPECT_EQ(fa.get(),expectedA);EXPECT_EQ(fb.get(),expectedB);EXPECT_EQ(arrived,2u);
}
}
