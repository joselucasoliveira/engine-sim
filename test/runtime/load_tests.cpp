#include "legacy_runtime.hpp"
#include "runtime.hpp"
#include "piston_engine_simulator.h"
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

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(es_runtime &rt) { return *rt.implementation.m_session->pull; }
    static Simulator &simulator(es_runtime &rt) { return *rt.implementation.m_session->prepared.session->simulator; }
};
}
namespace {
using namespace engine_sim;
namespace fs=std::filesystem;
struct Torque:atg_scs::ForceGenerator {
    atg_scs::RigidBody *body=nullptr;double value=0;
    void apply(atg_scs::SystemState *s) override {s->t[body->index]+=value;}
};
TEST(Load, AnalyticRotorObeysImpulseBudgetAndStaticFrictionInBothDirections) {
    for(double inertia:{.12,1.,7.}) for(double initial:{0.,-10.,10.,-.001,.001}) for(double cap:{0.,5.,10000.}) for(double force:{-8.,0.,8.}) {
        CheckedSolver solver;atg_scs::RigidBody body;body.I=inertia;body.m=1;body.v_theta=initial;
        atg_scs::RotationFrictionConstraint brake;brake.setBody(&body);brake.m_minTorque=-cap;brake.m_maxTorque=cap;
        Torque torque;torque.body=&body;torque.value=force;
        atg_scs::OptimizedNsvRigidBodySystem system;system.initialize(&solver);system.addRigidBody(&body);system.addConstraint(&brake);system.addForceGenerator(&torque);
        constexpr double dt=.01;const double predicted=initial+dt*force/inertia;
        const double expected=std::copysign(std::max(0.,std::abs(predicted)-dt*cap/inertia),predicted);
        system.process(dt,1);
        EXPECT_NEAR(body.v_theta,expected,1e-12);EXPECT_LE(std::abs(brake.F_t[0][0]),cap+1e-10);
        EXPECT_LE(brake.F_t[0][0]*body.v_theta,1e-10);
        if(force==0) EXPECT_LE(.5*inertia*(body.v_theta*body.v_theta-initial*initial),1e-10);
    }
}
using Handle=std::unique_ptr<es_runtime,decltype(&es_destroy)>;
void checked(es_status s,const es_error &e) {if(s!=ES_OK)throw std::runtime_error(e.message);}
es_telemetry telemetry(es_runtime *r) {es_telemetry t{};t.size=sizeof(t);t.abi=ES_ABI_V2;es_error e{};checked(es_get_telemetry(r,&t,&e),e);return t;}
es_controls vector(double load=0,double demand=.25,bool ignition=false,bool starter=false) {return {sizeof(es_controls),ES_ABI_V2,static_cast<std::uint32_t>(ignition),static_cast<std::uint32_t>(starter),demand,load,0};}
void controls(es_runtime *r,const es_controls &c) {es_error e{};checked(es_set_controls(r,&c,&e),e);}
std::vector<float> render(es_runtime *r,std::size_t count,std::size_t part=257) {
    std::vector<float> pcm(count);es_error e{};
    for(std::size_t n=0;n<count;) {const auto take=static_cast<std::uint32_t>(std::min(part,count-n));std::uint32_t written=99;checked(es_render(r,take,pcm.data()+n,take,&written,&e),e);if(written!=take)throw std::runtime_error("short successful render");n+=take;}return pcm;
}
class LoadEngine:public testing::Test {
protected:
    fs::path root;
    void SetUp() override {const auto *s=std::getenv("ENGINE_SIM_TEST_SOURCE");ASSERT_NE(s,nullptr);root=fs::u8path(s)/"examples/runtime/fixture";}
    Handle prepare(std::uint32_t rate=44100,std::uint32_t frequency=10000,fs::path path={}) {
        if(path.empty())path=root;es_config c{};es_error e{};checked(es_config_init(ES_ABI_V2,sizeof(c),&c,&e),e);c.output_hz=rate;c.simulation_hz=frequency;
        es_runtime *raw=nullptr;checked(es_create(&c,&raw,&e),e);Handle r(raw,es_destroy);
        checked(engine_sim::legacy_runtime::load(raw,(path/"main.mr").u8string().c_str(),(path/"library").u8string().c_str(),path.u8string().c_str(),&e),e);return r;
    }
};
TEST_F(LoadEngine, RequestedValueIsImmediateButReactionUsesTheEligibleCompletedTick) {
    EXPECT_NE(es_capabilities()&ES_CAP_LOAD,0u);auto r=prepare(44100,4000),reference=prepare(44100,4000);
    auto c=vector(5,.25,false,true);controls(r.get(),c);controls(reference.get(),c);
    EXPECT_EQ(telemetry(r.get()).requested_load_nm,5);EXPECT_EQ(telemetry(r.get()).load_reaction_nm,0);
    EXPECT_EQ(render(r.get(),1),render(reference.get(),1));EXPECT_EQ(telemetry(r.get()).ticks,0u);
    c.load_nm=12;controls(r.get(),c);controls(reference.get(),c);
    for(double value:{-1.,10001.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid=vector(value,.9,true,false);es_error e{};EXPECT_EQ(es_set_controls(r.get(),&invalid,&e),ES_INVALID);EXPECT_EQ(telemetry(r.get()).requested_load_nm,12);
    }
    EXPECT_EQ(render(r.get(),11),render(reference.get(),11));auto t=telemetry(r.get());EXPECT_EQ(t.ticks,1u);EXPECT_EQ(t.requested_load_nm,12);EXPECT_GT(t.load_reaction_nm,0);EXPECT_LE(t.load_reaction_nm,5+1e-9);
    EXPECT_EQ(render(r.get(),11),render(reference.get(),11));t=telemetry(r.get());EXPECT_EQ(t.ticks,2u);EXPECT_GT(t.load_reaction_nm,5);EXPECT_LE(t.load_reaction_nm,12+1e-9);
}
TEST_F(LoadEngine, SameStateCombustionOffCoastdownDissipatesAndSlowsTheRealShaft) {
    auto loaded=prepare(),free=prepare();controls(loaded.get(),vector(0,.25,false,true));controls(free.get(),vector(0,.25,false,true));
    EXPECT_EQ(render(loaded.get(),44100),render(free.get(),44100));const auto before=telemetry(loaded.get());ASSERT_GT(before.measured_rpm,100);
    controls(loaded.get(),vector(8));controls(free.get(),vector());double work=0,maxPower=0,maxReaction=0,previous=before.crank_rad_s;std::size_t ticks=0;
    auto &sim=RuntimeTestAccess::simulator(*loaded);
    RuntimeTestAccess::pull(*loaded).setTickObserver([&](const Simulator &){
        const auto &mechanics=static_cast<const PistonEngineSimulator &>(sim);const double reaction=mechanics.getExternalLoadReactionNm(),speed=sim.getEngine()->getOutputCrankshaft()->m_body.v_theta;
        maxPower=std::max(maxPower,reaction*speed);maxReaction=std::max(maxReaction,std::abs(reaction));work+=reaction*(previous+speed)*.5*sim.getTimestep();previous=speed;++ticks;
    });
    render(loaded.get(),8820);render(free.get(),8820);const auto a=telemetry(loaded.get()),b=telemetry(free.get());
    EXPECT_EQ(ticks,2000u);EXPECT_LT(a.measured_rpm,b.measured_rpm);EXPECT_LE(maxPower,1e-7);EXPECT_LE(maxReaction,8+1e-8);EXPECT_LT(work,0);
    EXPECT_EQ(a.requested_load_nm,8);EXPECT_GT(a.load_reaction_nm,0);EXPECT_EQ(a.drive_reaction_nm,0);
    EXPECT_EQ(sim.getTransmission()->getGear(),-1);EXPECT_EQ(sim.getTransmission()->getClutchPressure(),0);EXPECT_FALSE(sim.m_dyno.m_enabled);
    std::cout<<"coast initial_rpm="<<before.measured_rpm<<" loaded_rpm="<<a.measured_rpm<<" free_rpm="<<b.measured_rpm<<" brake_work_j="<<work<<" max_positive_power_w="<<maxPower<<'\n';
}
TEST_F(LoadEngine, RestHoldsIncipientTorqueAndHighLoadStallsWithoutRotationInjection) {
    for(double force:{-20.,0.,20.}) {
        auto r=prepare(44100,44100);auto &sim=RuntimeTestAccess::simulator(*r);Torque torque;torque.body=&sim.getEngine()->getOutputCrankshaft()->m_body;torque.value=force;
        sim.getSystem()->addForceGenerator(&torque);controls(r.get(),vector(10000));render(r.get(),100);
        const auto t=telemetry(r.get());EXPECT_NEAR(t.crank_rad_s,0,1e-8);EXPECT_LE(std::abs(t.load_reaction_nm),10000+1e-8);
        if(force) {EXPECT_LT(t.load_reaction_nm*force,0);EXPECT_GT(std::abs(t.load_reaction_nm),10);}
        sim.getSystem()->removeForceGenerator(&torque);
    }
    auto r=prepare();controls(r.get(),vector(0,.25,false,true));render(r.get(),44100);const auto initial=telemetry(r.get());ASSERT_GT(initial.measured_rpm,100);
    controls(r.get(),vector(10000));render(r.get(),4410);const auto stopped=telemetry(r.get());EXPECT_LT(stopped.measured_rpm,.001);EXPECT_LE(std::abs(stopped.load_reaction_nm),10000+1e-8);
    std::cout<<"stall initial_rpm="<<initial.measured_rpm<<" final_rpm="<<stopped.measured_rpm<<'\n';
}
TEST_F(LoadEngine, BrakeOpposesBothSignedDirectionsInTheCoupledEngine) {
    for(bool reverse:{false,true}) {
        auto r=prepare(44100,44100);controls(r.get(),vector(0,.25,false,true));render(r.get(),8820);auto &sim=RuntimeTestAccess::simulator(*r);
        // Test seam: reverse the complete consistent velocity state, not the
        // product's control path or crank angle. Gas/geometry remain identical.
        if(reverse)for(int i=0;i<sim.getSystem()->getRigidBodyCount();++i) {auto &b=*sim.getSystem()->getRigidBody(i);b.v_theta=-b.v_theta;b.v_x=-b.v_x;b.v_y=-b.v_y;}
        const double initial=sim.getEngine()->getOutputCrankshaft()->m_body.v_theta;ASSERT_GT(std::abs(initial),1);
        controls(r.get(),vector(8));render(r.get(),1);const auto t=telemetry(r.get());EXPECT_GT(t.crank_rad_s*initial,0);EXPECT_LT(t.load_reaction_nm*t.crank_rad_s,0);EXPECT_LE(std::abs(t.load_reaction_nm),8+1e-8);
    }
}
TEST_F(LoadEngine, StarterIgnitionAndLoadRemainIndependentPhysicalControls) {
    for(bool ignition:{false,true}) {
        auto r=prepare();auto &sim=RuntimeTestAccess::simulator(*r);unsigned sparks=0;RuntimeTestAccess::pull(*r).setTickObserver([&](const Simulator &){if(sim.getEngine()->getChamber(0)->popLitLastFrame())++sparks;});
        controls(r.get(),vector(0,.25,ignition,false));render(r.get(),441);EXPECT_LT(telemetry(r.get()).measured_rpm,.001);
        controls(r.get(),vector(1,.25,ignition,true));render(r.get(),44100);EXPECT_GT(telemetry(r.get()).measured_rpm,100);
        EXPECT_EQ(sim.m_starterMotor.m_enabled,true);EXPECT_EQ(sim.getEngine()->getIgnitionModule()->m_enabled,ignition);
        const auto prior=sparks;controls(r.get(),vector(1,.25,ignition,false));render(r.get(),2*44100);EXPECT_FALSE(sim.m_starterMotor.m_enabled);
        if(ignition) {EXPECT_GT(sparks-prior,0u);EXPECT_GT(telemetry(r.get()).measured_rpm,100);}else EXPECT_EQ(sparks,0u);
    }
}
TEST_F(LoadEngine, LoadedTimelineResetAndEveryPartitionReturnIdenticalPcmAndTelemetry) {
    for(auto rate:{44100u,48000u}) {
        auto a=prepare(rate),b=prepare(rate);
        const auto run=[&](es_runtime *r,std::size_t part) {
            std::vector<float> all;const std::array<std::size_t,8> edges{0,1,147,160,257,441,48000,96003};
            for(std::size_t i=0;i+1<edges.size();++i) {controls(r,vector(i%3?1.:0.,i%2?.3:.2,true,i<6));auto pcm=render(r,edges[i+1]-edges[i],part);all.insert(all.end(),pcm.begin(),pcm.end());}return all;
        };
        const auto expected=run(a.get(),65536);EXPECT_EQ(run(b.get(),1),expected);const auto ta=telemetry(a.get()),tb=telemetry(b.get());EXPECT_EQ(std::memcmp(&ta,&tb,sizeof(ta)),0);
        es_error e{};checked(engine_sim::legacy_runtime::reset(b.get(),&e),e);EXPECT_EQ(telemetry(b.get()).requested_load_nm,0);EXPECT_EQ(telemetry(b.get()).load_reaction_nm,0);EXPECT_EQ(run(b.get(),503),expected);
    }
}
TEST_F(LoadEngine, DemandPlateAndGovernorKeepTheirModelSemanticsWithAndWithoutLoad) {
    for(double load:{0.,8.}) {
        auto direct=prepare(44100,44100);controls(direct.get(),vector(load,.3,false,true));render(direct.get(),1);auto &d=RuntimeTestAccess::simulator(*direct);
        EXPECT_DOUBLE_EQ(d.getEngine()->getThrottle(),.7);EXPECT_DOUBLE_EQ(telemetry(direct.get()).plate_angle_rad,(1-.95*.7)*constants::pi/2);
        const auto governedRoot=fs::current_path()/(load?"governed-loaded":"governed-free");fs::create_directories(governedRoot);fs::copy(root,governedRoot,fs::copy_options::recursive|fs::copy_options::overwrite_existing);
        std::ifstream input(governedRoot/"main.mr",std::ios::binary|std::ios::ate);ASSERT_TRUE(input);std::string text(static_cast<std::size_t>(input.tellg()),'\0');input.seekg(0);ASSERT_TRUE(input.read(text.data(),text.size()));input.close();
        const std::string from="throttle_gamma: 1.0",to="throttle: governor(min_speed: 100 * u.rpm, max_speed: 1200 * u.rpm, min_v: -2.0, max_v: 2.0, k_s: 0.001, k_d: 10.0, gamma: 1.0)";
        const auto at=text.find(from);ASSERT_NE(at,std::string::npos);text.replace(at,from.size(),to);std::ofstream output(governedRoot/"main.mr",std::ios::binary);output<<text;output.close();ASSERT_TRUE(output);
        auto governed=prepare(44100,44100,governedRoot);auto &sim=RuntimeTestAccess::simulator(*governed);double plate=1,velocity=0,demand=.3;std::size_t checkedTicks=0;
        RuntimeTestAccess::pull(*governed).setTickObserver([&](const Simulator &){
            const double speed=sim.getEngine()->getSpeed(),target=units::rpm(100*(1-demand)+1200*demand),dt=sim.getTimestep();
            velocity+=dt*-(target*target-speed*speed)*.001-velocity*dt*10;velocity=std::clamp(velocity,-2.,2.);
            if(std::abs(speed)<.5*units::rpm(100)) {velocity=0;plate=1;}
            plate=std::clamp(plate+velocity*dt,0.,1.);EXPECT_NEAR(sim.getEngine()->getThrottle(),plate,1e-12);++checkedTicks;
        });
        controls(governed.get(),vector(load,demand,false,true));render(governed.get(),44100);demand=.8;controls(governed.get(),vector(load,demand,false,true));render(governed.get(),4410);
        EXPECT_EQ(checkedTicks,48510u);EXPECT_NE(sim.getEngine()->getThrottle(),1-std::pow(demand,1.));EXPECT_EQ(telemetry(governed.get()).requested_load_nm,load);
    }
}
}
