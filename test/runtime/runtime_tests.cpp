#include "runtime.hpp"
#include "simulation_fault.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>
#include <atomic>
#include <codecvt>
#include <locale>

namespace { std::atomic<bool> rejectNextAllocation{false}; }
void *operator new(std::size_t count) {
    if(rejectNextAllocation.exchange(false)) throw std::bad_alloc();
    if(auto *p=std::malloc(count?count:1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p,std::size_t) noexcept { std::free(p); }

namespace engine_sim {
struct ClockTestAccess {
    static void preload(PullEngine &pull, ClockCounts counts, std::uint32_t phase) {
        pull.m_clock.m_counts=counts;pull.m_clock.m_phase=phase;pull.m_clock.m_tickActive=phase!=0;
    }
};
struct RuntimeTestAccess {
    static PullEngine &pull(es_runtime &rt) { return *rt.implementation.m_session->pull; }
    static Simulator &simulator(es_runtime &rt) { return *rt.implementation.m_session->prepared.session->simulator; }
};
}
namespace {
namespace fs=std::filesystem;
using Handle=std::unique_ptr<es_runtime,decltype(&es_destroy)>;
class RuntimeApi:public testing::Test {
protected:
    fs::path root;
    es_config config{};
    es_error error{};
    void SetUp() override {
        const auto *source=std::getenv("ENGINE_SIM_TEST_SOURCE");ASSERT_NE(source,nullptr);
        root=fs::u8path(source)/"examples/runtime/fixture";
        ASSERT_EQ(es_config_init(ES_ABI_V1,sizeof(config),&config,&error),ES_OK);
    }
    Handle create(bool load=true) {
        es_runtime *rt=nullptr;
        if(es_create(&config,&rt,&error)!=ES_OK) throw std::runtime_error(error.message);
        Handle owned(rt,es_destroy);
        if(load && es_load(rt,(root/"main.mr").u8string().c_str(),(root/"library").u8string().c_str(),root.u8string().c_str(),&error)!=ES_OK)
            throw std::runtime_error(error.message);
        return owned;
    }
    es_telemetry telemetry(es_runtime *rt) {
        es_telemetry t{};t.size=sizeof(t);t.abi=ES_ABI_V1;
        if(es_get_telemetry(rt,&t,&error)!=ES_OK) throw std::runtime_error(error.message);
        return t;
    }
    void controls(es_runtime *rt,double demand=.25,bool starter=true) {
        es_controls c{sizeof(c),ES_ABI_V1,1,static_cast<std::uint32_t>(starter),demand,0,0};
        ASSERT_EQ(es_set_controls(rt,&c,&error),ES_OK)<<error.message;
    }
    std::vector<float> render(es_runtime *rt,std::size_t count) {
        std::vector<float> pcm(count);
        for(std::size_t n=0;n<count;) {
            const auto part=static_cast<std::uint32_t>(std::min<std::size_t>(ES_MAX_FRAMES,count-n));std::uint32_t written=99;
            if(es_render(rt,part,pcm.data()+n,part,&written,&error)!=ES_OK || written!=part) throw std::runtime_error(error.message);
            n+=part;
        }
        return pcm;
    }
};
TEST_F(RuntimeApi, ExtensiblePrefixesVersionsAndDefaultsKeepTailsUntouched) {
    struct Large { es_config config; std::array<unsigned char,16> tail; } large;
    std::memset(&large,0xa5,sizeof(large));
    ASSERT_EQ(es_config_init(ES_ABI_V1,sizeof(large),&large.config,&error),ES_OK);
    EXPECT_EQ(large.config.size,sizeof(large));EXPECT_EQ(large.config.seed,1u);EXPECT_EQ(large.config.output_hz,44100u);
    EXPECT_TRUE(std::all_of(large.tail.begin(),large.tail.end(),[](auto b){return b==0xa5;}));
    const auto saved=large;
    EXPECT_EQ(es_config_init(ES_ABI_V1,sizeof(es_config)-1,&large.config,&error),ES_CAPACITY);
    EXPECT_EQ(std::memcmp(&large,&saved,sizeof(large)),0);
    EXPECT_EQ(es_config_init(0x00020000,sizeof(large),&large.config,&error),ES_VERSION);
    EXPECT_EQ(es_config_init(0x00010001,sizeof(large),&large.config,&error),ES_VERSION);
    EXPECT_EQ(std::memcmp(&large,&saved,sizeof(large)),0);
    auto rt=create(false);
    struct LargeTelemetry { es_telemetry value; std::array<unsigned char,16> tail; } out;
    std::memset(&out,0xcc,sizeof(out));out.value.size=sizeof(out);out.value.abi=ES_ABI_V1;
    ASSERT_EQ(es_get_telemetry(rt.get(),&out.value,&error),ES_OK);
    EXPECT_EQ(out.value.size,sizeof(out));EXPECT_EQ(out.value.state,ES_EMPTY);EXPECT_EQ(out.value.output_hz,44100u);EXPECT_EQ(out.value.simulation_hz,0u);
    EXPECT_TRUE(std::all_of(out.tail.begin(),out.tail.end(),[](auto b){return b==0xcc;}));
    out.value.size=8;const auto before=out;
    EXPECT_EQ(es_get_telemetry(rt.get(),&out.value,&error),ES_CAPACITY);EXPECT_EQ(std::memcmp(&out,&before,sizeof(out)),0);
}
TEST_F(RuntimeApi, InvalidCreateAndUnsupportedCapabilitiesNeverReturnAHandle) {
    EXPECT_EQ(es_capabilities() & (ES_CAP_FREE|ES_CAP_44100|ES_CAP_RESET),ES_CAP_FREE|ES_CAP_44100|ES_CAP_RESET);
    const auto reject=[&](es_status expected) {
        es_runtime sentinel(config);es_runtime *out=&sentinel;
        EXPECT_EQ(es_create(&config,&out,&error),expected)<<error.message;EXPECT_EQ(out,nullptr);EXPECT_NE(error.message[0],0);
    };
    config.flags=1;reject(ES_UNSUPPORTED);config.flags=0;
    config.output_hz=32000;reject(ES_UNSUPPORTED);config.output_hz=0;reject(ES_INVALID);config.output_hz=44100;
    config.mode=ES_DRIVEN;reject(ES_UNSUPPORTED);config.mode=2;reject(ES_INVALID);config.mode=ES_FREE;
    config.simulation_hz=3999;reject(ES_INVALID);config.simulation_hz=96001;reject(ES_INVALID);config.simulation_hz=0;
    for(auto value:{-1.,10001.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) { config.starter_torque_nm=value;reject(ES_INVALID); }
    config.starter_torque_nm=0;config.size=8;reject(ES_CAPACITY);config.size=sizeof(config);config.abi=0;reject(ES_VERSION);
    EXPECT_EQ(es_config_init(ES_ABI_V1,sizeof(config),nullptr,&error),ES_INVALID);
    EXPECT_EQ(es_create(nullptr,nullptr,nullptr),ES_INVALID);es_destroy(nullptr);
}
TEST_F(RuntimeApi, StateAndSpanValidationNeverWritesPcmOrAdvances) {
    auto rt=create(false);std::uint32_t written=99;std::array<float,8> pcm;pcm.fill(42);
    EXPECT_EQ(es_render(rt.get(),0,nullptr,0,&written,&error),ES_STATE);EXPECT_EQ(written,0u);
    es_controls c{sizeof(c),ES_ABI_V1,0,0,0,0,0};EXPECT_EQ(es_set_controls(rt.get(),&c,&error),ES_STATE);
    EXPECT_EQ(es_reset(rt.get(),&error),ES_STATE);
    ASSERT_EQ(es_load(rt.get(),(root/"main.mr").u8string().c_str(),(root/"library").u8string().c_str(),root.u8string().c_str(),&error),ES_OK);
    EXPECT_EQ(es_render(rt.get(),0,nullptr,0,&written,&error),ES_OK);EXPECT_EQ(telemetry(rt.get()).frames,0u);
    EXPECT_EQ(es_render(rt.get(),8,pcm.data(),7,&written,&error),ES_CAPACITY);EXPECT_EQ(written,0u);
    EXPECT_EQ(es_render(rt.get(),ES_MAX_FRAMES+1,pcm.data(),UINT32_MAX,&written,&error),ES_CAPACITY);
    EXPECT_EQ(es_render(rt.get(),1,nullptr,1,&written,&error),ES_INVALID);
    EXPECT_EQ(es_render(rt.get(),1,pcm.data(),8,nullptr,&error),ES_INVALID);
    EXPECT_EQ(es_render(nullptr,1,pcm.data(),8,&written,&error),ES_INVALID);
    EXPECT_TRUE(std::all_of(pcm.begin(),pcm.end(),[](auto v){return v==42;}));EXPECT_EQ(telemetry(rt.get()).frames,0u);
    ASSERT_EQ(es_unload(rt.get(),nullptr),ES_OK);EXPECT_EQ(telemetry(rt.get()).state,ES_EMPTY);
    EXPECT_EQ(es_unload(rt.get(),nullptr),ES_OK);EXPECT_EQ(es_unload(nullptr,&error),ES_INVALID);
}
TEST_F(RuntimeApi, RejectedControlsPreserveFullVectorAndErrorStorageIsIndependent) {
    auto a=create(),b=create();controls(a.get());controls(b.get());
    const es_controls valid{sizeof(es_controls),ES_ABI_V1,1,1,.25,0,0};
    auto c=valid;es_error first{};
    c.ignition=2;EXPECT_EQ(es_set_controls(a.get(),&c,&first),ES_INVALID);const auto previous=first;
    c=valid;c.demand=std::numeric_limits<double>::quiet_NaN();EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_INVALID);
    c=valid;c.starter=2;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_INVALID);
    c=valid;c.demand=1.01;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_INVALID);
    c=valid;c.load_nm=10001;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_INVALID);
    c=valid;c.driven_rpm=1;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_UNSUPPORTED);
    c=valid;c.load_nm=-1;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_INVALID);
    c=valid;c.driven_rpm=std::numeric_limits<double>::infinity();EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_INVALID);
    c=valid;c.size=8;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_CAPACITY);
    c=valid;c.abi=0;EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_VERSION);
    EXPECT_EQ(render(a.get(),4410),render(b.get(),4410));EXPECT_EQ(std::memcmp(&first,&previous,sizeof(first)),0);
    EXPECT_EQ(error.message[0],0);
}
TEST_F(RuntimeApi, SameControlTimelineMatchesEveryPartitionAndTelemetryFormula) {
    const auto run=[&](const std::vector<std::size_t> &parts) {
        auto rt=create();std::vector<float> result;
        const std::array<std::size_t,9> edges{0,1,147,160,257,441,47999,48007,88201};std::size_t index=0;
        for(std::size_t edge=0;edge+1<edges.size();++edge) {
            controls(rt.get(),edge%2?.25:.4,edges[edge]<47999);
            for(auto n=edges[edge];n<edges[edge+1];) {
                const auto count=std::min(parts[index++%parts.size()],edges[edge+1]-n);auto pcm=render(rt.get(),count);result.insert(result.end(),pcm.begin(),pcm.end());n+=count;
                const auto t=telemetry(rt.get());EXPECT_EQ(t.frames,n);EXPECT_EQ(t.native_frames,n);EXPECT_EQ(t.ticks,n*10000/44100);
                EXPECT_DOUBLE_EQ(t.measured_rpm,std::abs(t.crank_rad_s)*60/(2*constants::pi));
                EXPECT_GE(t.cycle_rad,0);EXPECT_LT(t.cycle_rad,4*constants::pi);EXPECT_TRUE(std::isfinite(t.plate_angle_rad));
            }
        }
        const auto t=telemetry(rt.get());return std::make_pair(result,std::array<double,4>{t.crank_rad_s,t.cycle_rad,t.plate_angle_rad,t.displacement_m3});
    };
    const auto reference=run({65536});EXPECT_EQ(run({256}),reference);EXPECT_EQ(run({7,251,31,1,503}),reference);EXPECT_EQ(run({1}),reference);
}
TEST_F(RuntimeApi, FailedReplacementAndInvalidPathsPreserveContinuation) {
    auto a=create(),b=create();controls(a.get());controls(b.get());EXPECT_EQ(render(a.get(),735),render(b.get(),735));
    EXPECT_EQ(es_load(a.get(),"relative.mr",root.u8string().c_str(),root.u8string().c_str(),&error),ES_INVALID);
    const char invalid[]{static_cast<char>(0xc0),static_cast<char>(0x80),0};
    EXPECT_EQ(es_load(a.get(),invalid,root.u8string().c_str(),root.u8string().c_str(),&error),ES_INVALID);
    EXPECT_EQ(es_load(a.get(),(root/"absent.mr").u8string().c_str(),root.u8string().c_str(),root.u8string().c_str(),&error),ES_RESOURCE);
    const auto path=fs::absolute("broken.mr");{std::ofstream out(path);out<<"import \"absent_import_for_runtime_test.mr\"\n";}
    EXPECT_EQ(es_load(a.get(),path.u8string().c_str(),(root/"library").u8string().c_str(),root.u8string().c_str(),&error),ES_RESOURCE);
    {std::ofstream out(path);out<<"this is broken (";}
    EXPECT_EQ(es_load(a.get(),path.u8string().c_str(),(root/"library").u8string().c_str(),root.u8string().c_str(),&error),ES_SCRIPT);
    EXPECT_EQ(render(a.get(),4410),render(b.get(),4410));EXPECT_EQ(telemetry(a.get()).state,ES_READY);
}
TEST_F(RuntimeApi, ComputationFaultPublishesExactPrefixAndLastCoherentSnapshot) {
    config.simulation_hz=44100;auto a=create(),b=create();controls(a.get());controls(b.get());
    const auto expected=render(b.get(),37);const auto coherent=telemetry(b.get());
    unsigned ticks=0;engine_sim::RuntimeTestAccess::pull(*a).setTickObserver([&](const Simulator &){if(++ticks==38) throw engine_sim::SimulationFault("injected tick fault");});
    std::array<float,66> pcm;pcm.fill(42);std::uint32_t written=99;
    EXPECT_EQ(es_render(a.get(),64,pcm.data()+1,64,&written,&error),ES_RENDER_FAULT);ASSERT_EQ(written,37u);
    EXPECT_TRUE(std::equal(expected.begin(),expected.end(),pcm.begin()+1));EXPECT_EQ(pcm[0],42);
    EXPECT_TRUE(std::all_of(pcm.begin()+38,pcm.end(),[](auto v){return v==42;}));
    const auto faulted=telemetry(a.get());EXPECT_EQ(faulted.state,ES_FAULTED);EXPECT_EQ(faulted.frames,37u);EXPECT_EQ(faulted.ticks,coherent.ticks);EXPECT_DOUBLE_EQ(faulted.crank_rad_s,coherent.crank_rad_s);EXPECT_DOUBLE_EQ(faulted.cycle_rad,coherent.cycle_rad);
    EXPECT_EQ(es_render(a.get(),0,nullptr,0,&written,&error),ES_STATE);EXPECT_EQ(written,0u);
    es_controls c{sizeof(c),ES_ABI_V1,0,0,0,0,0};EXPECT_EQ(es_set_controls(a.get(),&c,&error),ES_STATE);
    EXPECT_EQ(es_unload(a.get(),&error),ES_OK);EXPECT_EQ(telemetry(a.get()).frames,0u);
}
TEST_F(RuntimeApi, CounterCapacityIsRejectedBeforeAnySourceWork) {
    config.simulation_hz=44100;auto rt=create();auto &pull=engine_sim::RuntimeTestAccess::pull(*rt);
    engine_sim::ClockTestAccess::preload(pull,{UINT64_MAX-2,UINT64_MAX-2,UINT64_MAX-2},0);
    std::array<float,3> pcm{42,42,42};std::uint32_t written=99;
    EXPECT_EQ(es_render(rt.get(),3,pcm.data(),3,&written,&error),ES_CAPACITY);EXPECT_EQ(written,0u);EXPECT_EQ(pull.counts().frames,UINT64_MAX-2);EXPECT_EQ(pcm[0],42);EXPECT_EQ(pcm[2],42);
    EXPECT_EQ(telemetry(rt.get()).state,ES_READY);
    EXPECT_EQ(es_render(rt.get(),2,pcm.data(),3,&written,&error),ES_OK);EXPECT_EQ(written,2u);EXPECT_EQ(pull.counts().frames,UINT64_MAX);
    EXPECT_EQ(es_render(rt.get(),0,nullptr,0,&written,&error),ES_OK);
    EXPECT_EQ(es_render(rt.get(),1,pcm.data(),3,&written,&error),ES_CAPACITY);EXPECT_EQ(written,0u);
}
TEST_F(RuntimeApi, AllocationAndUnexpectedExceptionsRemainInsideTheBoundary) {
    es_runtime *out=nullptr;
    rejectNextAllocation.store(true);
    const auto status=es_create(&config,&out,&error);
    rejectNextAllocation.store(false);
    EXPECT_EQ(status,ES_NO_MEMORY);EXPECT_EQ(out,nullptr);EXPECT_NE(error.message[0],0);
    config.simulation_hz=44100;
    for(const auto kind:{0,1,2}) {
        auto rt=create();controls(rt.get());unsigned ticks=0;
        if(kind==0) engine_sim::RuntimeTestAccess::simulator(*rt).getEngine()->getCrankshaft(0)->m_body.theta=std::numeric_limits<double>::infinity();
        else engine_sim::RuntimeTestAccess::pull(*rt).setTickObserver([&](const Simulator &){
            if(++ticks==6) { if(kind==1) throw std::bad_alloc(); throw 17; }
        });
        std::array<float,20> pcm;pcm.fill(42);std::uint32_t written=99;
        EXPECT_EQ(es_render(rt.get(),20,pcm.data(),20,&written,&error),kind==0?ES_RENDER_FAULT:kind==1?ES_NO_MEMORY:ES_INTERNAL);
        EXPECT_EQ(written,kind==0?0u:5u);
        EXPECT_TRUE(std::all_of(pcm.begin()+written,pcm.end(),[](auto x){return x==42;}));
        const auto t=telemetry(rt.get());EXPECT_EQ(t.state,ES_FAULTED);EXPECT_EQ(t.frames,written);
    }
}
TEST_F(RuntimeApi, DiagnosticTruncationKeepsCompleteUtf8AndNoGlobalLastError) {
    auto rt=create(false);
    std::string name;for(int i=0;i<400;++i) name+=u8"é";
    const auto missing=(root/fs::u8path(name)).u8string();
    std::memset(&error,0xcc,sizeof(error));
    EXPECT_EQ(es_load(rt.get(),missing.c_str(),root.u8string().c_str(),root.u8string().c_str(),&error),ES_RESOURCE);
    EXPECT_NE(std::strstr(error.message,"resources:"),nullptr);
    ASSERT_NE(std::memchr(error.message,0,sizeof(error.message)),nullptr);
    std::wstring_convert<std::codecvt_utf8<char32_t>,char32_t> decoder;
    EXPECT_NO_THROW(decoder.from_bytes(error.message));EXPECT_EQ(decoder.converted(),std::strlen(error.message));
    const auto saved=error;EXPECT_EQ(es_unload(rt.get(),nullptr),ES_OK);EXPECT_EQ(std::memcmp(&error,&saved,sizeof(error)),0);
}
TEST_F(RuntimeApi, RealCombustionAndPlateLinkageContinueAfterStarterRelease) {
    auto rt=create();auto &sim=engine_sim::RuntimeTestAccess::simulator(*rt);unsigned firing=0;
    engine_sim::RuntimeTestAccess::pull(*rt).setTickObserver([&](const Simulator &){if(sim.getEngine()->getChamber(0)->popLitLastFrame()) ++firing;});
    controls(rt.get(),.25,true);render(rt.get(),44100);const auto before=firing;
    controls(rt.get(),.25,false);const auto pcm=render(rt.get(),4*44100);const auto t=telemetry(rt.get());
    EXPECT_GT(firing-before,5u);EXPECT_GT(t.measured_rpm,100);EXPECT_NE(t.plate_angle_rad,0);
    double energy=0;for(auto x:pcm) energy+=x*x;EXPECT_GT(energy,1e-6);
    EXPECT_EQ(sim.getTransmission()->getGear(),-1);EXPECT_DOUBLE_EQ(sim.getTransmission()->getClutchPressure(),0);
    EXPECT_FALSE(sim.m_dyno.m_enabled);EXPECT_FALSE(sim.m_starterMotor.m_enabled);
}

TEST_F(RuntimeApi, FacadeMatchesPreparedPullAcrossFrequenciesAndControlEdges) {
    for(const auto frequency:{4000u,30000u,96000u}) {
        config.simulation_hz=frequency;config.starter_torque_nm=25;
        config.seed=UINT64_MAX;
        auto rt=create();
        auto direct=engine_sim::prepareEngine({root/"main.mr",root/"library",root},{frequency,ES_FREE,25,UINT64_MAX});
        es_controls requested{sizeof(es_controls),ES_ABI_V1,1,1,.25,0,0};
        auto &sim=*direct.session->simulator;
        engine_sim::PullEngine pull(sim,[&]{sim.getEngine()->setSpeedControl(requested.demand);sim.getEngine()->getIgnitionModule()->m_enabled=requested.ignition!=0;sim.m_starterMotor.m_enabled=requested.starter!=0;});
        const std::array<std::size_t,5> edges{0,147,257,441,44101};
        for(std::size_t i=0;i+1<edges.size();++i) {
            requested.demand=i%2?.4:.25;
            requested.starter=i<3;
            ASSERT_EQ(es_set_controls(rt.get(),&requested,&error),ES_OK);
            const auto count=edges[i+1]-edges[i];
            const auto actual=render(rt.get(),count);std::vector<float> expected(count);
            pull.render(expected.data(),expected.size());EXPECT_EQ(actual,expected);
            const auto t=telemetry(rt.get());EXPECT_EQ(t.frames,pull.counts().frames);EXPECT_EQ(t.ticks,pull.counts().ticks);
            EXPECT_DOUBLE_EQ(t.crank_rad_s,sim.getEngine()->getCrankshaft(0)->m_body.v_theta);
            EXPECT_DOUBLE_EQ(t.cycle_rad,sim.getEngine()->getCrankshaft(0)->getCycleAngle());
            EXPECT_DOUBLE_EQ(t.displacement_m3,sim.getEngine()->getDisplacement());
        }
    }
}

}
