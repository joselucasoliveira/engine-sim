#include "sample_clock.h"
#include "runtime.hpp"
#include "simulation_fault.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <vector>

namespace engine_sim {
struct ClockTestAccess {
    static void preload(SampleClock &clock, ClockCounts counts, std::uint32_t tickPhase, std::uint32_t nativePhase) {
        clock.m_counts=counts;clock.m_outputTickPhase=tickPhase;clock.m_outputNativePhase=nativePhase;
        clock.m_tickActive=tickPhase!=0;clock.m_haveNative=true;
        clock.m_nativeInterpolationPhase=static_cast<std::uint32_t>((counts.nativeFrames%44100)*clock.m_frequency%44100);
    }
};
struct RuntimeTestAccess { static PullEngine &pull(es_runtime &rt) {return *rt.implementation.m_session->pull;} };
}
namespace {
using namespace engine_sim;
constexpr std::uint64_t S=44100,R=48000;
std::int16_t tag(std::uint64_t n) {return static_cast<std::int16_t>(static_cast<int>(n*97%60001)-30000);}
struct Source:ClockSource {
    std::function<std::int16_t(std::uint64_t)> signal=tag;
    std::uint64_t samples=0,ticks=0,starts=0,failSample=UINT64_MAX,failTick=UINT64_MAX;
    int requested=0,applied=0;
    bool record=true;
    std::vector<double> inputs;
    std::vector<std::pair<std::uint64_t,int>> controls;
    std::vector<std::pair<char,std::uint64_t>> events;
    static double endpoint(std::uint64_t tick) {return static_cast<double>(tick%257);}
    void latch() override {applied=requested;if(record){controls.emplace_back(starts,applied);events.emplace_back('L',starts);}++starts;}
    bool tick(double *out, std::size_t capacity) override {
        if (out == nullptr || capacity == 0) return false;
        ++ticks;
        out[0] = endpoint(ticks);
        if (record) events.emplace_back('T',ticks);
        return ticks != failTick;
    }
    bool native(AcousticExcitation input,std::int16_t &out) override {
        const auto n=samples++;if(record){inputs.push_back(input.data[0]);events.emplace_back('N',n);}out=signal(n);return n!=failSample;
    }
};
float expected(std::uint64_t n,const std::function<std::int16_t(std::uint64_t)> &signal) {
    // Direct absolute-index equation, independent of the scheduler's bounded phases.
    const auto j=n*147/160;const double b=static_cast<double>(n*147%160)/160;
    return static_cast<float>(((1-b)*signal(j?j-1:0)+b*signal(j))/32768.0);
}
TEST(Rate, DcRampImpulseExtremaAndInitialHistoryFollowAbsoluteSampleEquation) {
    std::vector<std::function<std::int16_t(std::uint64_t)>> signals{
        [](auto){return std::int16_t{12345};},[](auto){return std::int16_t{-32768};},[](auto){return std::int16_t{32767};},
        [](auto n){return static_cast<std::int16_t>(static_cast<int>(n%50000)-25000);},
        [](auto n){return std::int16_t(n==1?32767:0);},
        [](auto n){return std::int16_t(n%2?32767:-32768);},tag};
    for(const auto &signal:signals) {
        Source source;source.signal=signal;source.record=false;SampleClock clock(30000,{0.},source,SampleClock::Profile::Public,R);
        std::vector<float> out(48007);
        for(std::size_t n=0;n<out.size();) {const auto count=std::min<std::size_t>(257,out.size()-n);clock.render(out.data()+n,count);n+=count;}
        for(std::size_t n=0;n<out.size();++n) ASSERT_EQ(out[n],expected(n,signal))<<n;
        EXPECT_EQ(clock.counts().nativeFrames,(out.size()*147+159)/160);EXPECT_EQ(clock.counts().ticks,out.size()*30000/R);
        EXPECT_EQ(out.front(),static_cast<float>(signal(0))/32768);EXPECT_EQ(out[1],out.front());
    }
}
TEST(Rate, RationalEventsControlsAndExhaustInterpolationNeverAdvanceFuturePhysics) {
    for(const auto frequency:{4000u,10000u,30000u,44100u,48000u,96000u}) {
        SCOPED_TRACE(frequency);Source source;SampleClock clock(frequency,{0.},source,SampleClock::Profile::Public,R);
        const std::array<std::size_t,9> edges{0,1,147,160,257,441,47999,48007,96003};std::vector<float> out(edges.back());
        for(std::size_t e=0;e+1<edges.size();++e) {
            source.requested=static_cast<int>(e+1);clock.render(out.data()+edges[e],edges[e+1]-edges[e]);
            EXPECT_EQ(clock.counts().ticks,edges[e+1]*frequency/R);EXPECT_EQ(clock.counts().nativeFrames,(edges[e+1]*S+R-1)/R);
        }
        ASSERT_EQ(source.inputs.size(),clock.counts().nativeFrames);
        for(std::uint64_t n=0;n<source.inputs.size();++n) {
            const auto k=n*frequency/S;const double a=static_cast<double>(n*frequency%S)/S;
            const double value=(1-a)*(k?Source::endpoint(k-1):0)+a*Source::endpoint(k);
            ASSERT_EQ(source.inputs[n],value)<<n;
        }
        for(const auto &latch:source.controls) {
            int value=0;for(std::size_t e=0;e+1<edges.size();++e) if(edges[e]*frequency<=latch.first*R) value=static_cast<int>(e+1);
            ASSERT_EQ(latch.second,value)<<latch.first;
        }
        for(std::size_t i=1;i<source.events.size();++i) {
            const auto a=source.events[i-1],b=source.events[i];
            const auto time=[&](auto event){return event.second*(event.first=='N'?frequency:S);};
            ASSERT_LE(time(a),time(b));
            if(time(a)==time(b)) {const auto order=[](char c){return c=='T'?0:c=='N'?1:2;};ASSERT_LT(order(a.first),order(b.first));}
        }
        EXPECT_EQ(source.starts,(out.size()*frequency+R-1)/R);
        if(frequency==96000) {Source negative;negative.record=false;SampleClock native(frequency,{0.},negative);std::array<float,12> temp;native.render(temp.data(),temp.size());EXPECT_EQ(native.counts().ticks,26u);EXPECT_NE(native.counts().ticks,12*frequency/R);}
    }
}
TEST(Rate, LongPhaseCyclesAndArbitraryPartitionsAreBitIdentical) {
    const auto run=[](const std::vector<std::size_t> &parts) {
        Source source;source.record=false;SampleClock clock(96000,{0.},source,SampleClock::Profile::Public,R);std::vector<float> pcm(1000007);
        for(std::size_t n=0,i=0;n<pcm.size();++i) {
            const auto count=std::min(parts[i%parts.size()],pcm.size()-n);clock.render(pcm.data()+n,count);n+=count;
            EXPECT_EQ(clock.counts().ticks,n*2);EXPECT_EQ(clock.counts().nativeFrames,(n*147+159)/160);
        }
        return pcm;
    };
    const auto reference=run({65536});EXPECT_EQ(run({1}),reference);EXPECT_EQ(run({7,251,31,1,503}),reference);
    Source source;SampleClock clock(10000,{0.},source,SampleClock::Profile::Public,R);clock.render(nullptr,0);EXPECT_EQ(source.samples,0u);EXPECT_EQ(source.starts,0u);
    EXPECT_THROW(SampleClock(10000,{0.},source,SampleClock::Profile::Public,32000),std::invalid_argument);
    EXPECT_THROW(SampleClock(10000,{0.},source,SampleClock::Profile::Desktop,R),std::invalid_argument);
}
TEST(Rate, TonePitchDelayAndPassbandDroopAgreeWithAnalyticLinearInterpolation) {
    constexpr std::size_t count=3*R,begin=R,length=2*R;
    for(const auto hz:{997.,8000.,16000.,20000.}) {
        Source source;source.record=false;source.signal=[hz](auto n){return static_cast<std::int16_t>(std::lround(.7*32767*std::sin(2*constants::pi*hz*n/S)));};
        SampleClock clock(10000,{0.},source,SampleClock::Profile::Public,R);std::vector<float> pcm(count);
        for(std::size_t n=0;n<count;) {const auto take=std::min<std::size_t>(65536,count-n);clock.render(pcm.data()+n,take);n+=take;}
        std::complex<double> fitted{},wrong{};
        for(std::size_t n=begin;n<count;++n) {
            const double angle=2*constants::pi*hz*n/R,other=2*constants::pi*(hz+1)*n/R;
            fitted+=static_cast<double>(pcm[n])*std::complex<double>(std::cos(angle),-std::sin(angle));
            wrong+=static_cast<double>(pcm[n])*std::complex<double>(std::cos(other),-std::sin(other));
        }
        const auto gain=fitted*(2.0/length)/std::complex<double>(0,-.7*32767/32768)*std::polar(1.,2*constants::pi*hz/S);
        const double x=constants::pi*hz/S,analytic=std::pow(std::sin(x)/x,2);
        const double db=20*std::log10(std::abs(gain)),theory=20*std::log10(analytic);
        std::cout<<"tone="<<hz<<" measured_db="<<db<<" analytic_db="<<theory<<" residual_phase="<<std::arg(gain)<<'\n';
        EXPECT_NEAR(db,theory,.002);EXPECT_NEAR(std::arg(gain),0,.0002);EXPECT_LT(std::abs(wrong),std::abs(fitted)*.001);
    }
}
TEST(Rate, NativeAndTickFaultsCommitOnlyCompleteHostIntervals) {
    for(const bool native:{false,true}) {
        Source source;source.record=false;if(native)source.failSample=37;else source.failTick=5;
        SampleClock clock(96000,{0.},source,SampleClock::Profile::Public,R);std::array<float,80> out;out.fill(42);
        EXPECT_THROW(clock.render(out.data(),out.size()),SimulationFault);
        const auto prefix=native?40u:2u;EXPECT_EQ(clock.lastWritten(),prefix);EXPECT_EQ(clock.counts().frames,prefix);EXPECT_EQ(clock.counts().ticks,prefix*2);EXPECT_EQ(clock.counts().nativeFrames,(prefix*147+159)/160);
        for(std::size_t n=0;n<prefix;++n)EXPECT_EQ(out[n],expected(n,source.signal));
        EXPECT_TRUE(std::all_of(out.begin()+prefix,out.end(),[](auto value){return value==42;}));EXPECT_TRUE(clock.faulted());
    }
}
TEST(Rate, CounterOverflowAndZeroWorkAreCheckedBeforeMutation) {
    Source source;source.record=false;SampleClock clock(48000,{0.},source,SampleClock::Profile::Public,R);std::array<float,8> out;out.fill(42);
    ClockTestAccess::preload(clock,{UINT64_MAX-1,UINT64_MAX-1,UINT64_MAX-1},0,0);
    EXPECT_THROW(clock.render(out.data(),2),std::overflow_error);EXPECT_EQ(source.samples,0u);EXPECT_EQ(out[0],42);
    clock.render(out.data(),1);EXPECT_EQ(clock.counts().frames,UINT64_MAX);EXPECT_EQ(clock.counts().ticks,UINT64_MAX);EXPECT_EQ(clock.counts().nativeFrames,UINT64_MAX);
    clock.render(nullptr,0);EXPECT_THROW(clock.render(out.data(),1),std::overflow_error);
    Source other;SampleClock limited(96000,{0.},other,SampleClock::Profile::Public,R);
    ClockTestAccess::preload(limited,{7,UINT64_MAX-1,7},0,300);
    EXPECT_THROW(limited.render(out.data(),1),std::overflow_error);EXPECT_EQ(other.samples,0u);
    // With no native point strictly inside this interval, a full native
    // counter still permits one host frame. The right-edge point is deferred.
    Source pending;pending.record=false;SampleClock edge(48000,{0.},pending,SampleClock::Profile::Public,R);
    ClockTestAccess::preload(edge,{159,159,UINT64_MAX},0,3900);
    edge.render(out.data(),1);EXPECT_EQ(edge.counts().nativeFrames,UINT64_MAX);EXPECT_EQ(pending.samples,0u);
    EXPECT_THROW(edge.render(out.data(),1),std::overflow_error);EXPECT_EQ(pending.samples,0u);
}
using Handle=std::unique_ptr<es_runtime,decltype(&es_destroy)>;
Handle instance(std::uint32_t rate,std::uint32_t frequency=10000) {
    es_config config;es_error error{};if(es_config_init(ES_ABI_V2,sizeof(config),&config,&error))throw std::runtime_error(error.message);config.output_hz=rate;config.simulation_hz=frequency;
    es_runtime *raw=nullptr;if(es_create(&config,&raw,&error))throw std::runtime_error(error.message);Handle owned(raw,es_destroy);
    const auto *source=std::getenv("ENGINE_SIM_TEST_SOURCE");if(!source)throw std::runtime_error("fixture source missing");const auto root=std::filesystem::u8path(source)/"examples/runtime/fixture";
    if(es_load_json_file(raw,(root/"main.json").u8string().c_str(),root.u8string().c_str(),nullptr,&error))throw std::runtime_error(error.message);return owned;
}
es_telemetry telemetry(es_runtime *rt) {es_telemetry t{};t.size=sizeof(t);t.abi=ES_ABI_V2;es_error e{};if(es_get_telemetry(rt,&t,&e))throw std::runtime_error(e.message);return t;}
std::vector<float> render(es_runtime *rt,std::size_t count,std::size_t part=257) {
    std::vector<float> pcm(count);es_error e{};
    for(std::size_t n=0;n<count;) {const auto take=static_cast<std::uint32_t>(std::min(part,count-n));std::uint32_t written;if(es_render(rt,take,pcm.data()+n,take,&written,&e)||written!=take)throw std::runtime_error(e.message);n+=take;}return pcm;
}
TEST(Rate, RealEngineMatchesNativePcmOnTheSamePhysicalControlTimelineAndReset) {
    EXPECT_NE(es_capabilities()&ES_CAP_48000,0u);
    for(const auto frequency:{4000u,10000u,30000u,96000u}) {
        auto native=instance(S,frequency),converted=instance(R,frequency);std::vector<float> nativePcm,actual;
        for(unsigned interval=0;interval<125;++interval) {
            es_controls c{sizeof(c),ES_ABI_V2,1,interval<100?1u:0u,interval<25?.2:.3,0,0};es_error e{};
            ASSERT_EQ(es_set_controls(native.get(),&c,&e),ES_OK);ASSERT_EQ(es_set_controls(converted.get(),&c,&e),ES_OK);
            auto a=render(native.get(),441,31),b=render(converted.get(),480,7);nativePcm.insert(nativePcm.end(),a.begin(),a.end());actual.insert(actual.end(),b.begin(),b.end());
            const auto ta=telemetry(native.get()),tb=telemetry(converted.get());EXPECT_EQ(ta.ticks,tb.ticks);EXPECT_EQ(ta.native_frames,tb.native_frames);EXPECT_DOUBLE_EQ(ta.crank_rad_s,tb.crank_rad_s);EXPECT_DOUBLE_EQ(ta.cycle_rad,tb.cycle_rad);
        }
        const auto signal=[&](std::uint64_t n){return static_cast<std::int16_t>(std::lround(nativePcm[n]*32768.));};
        for(std::size_t n=0;n<actual.size();++n)ASSERT_EQ(actual[n],expected(n,signal))<<frequency<<' '<<n;
        es_error e{};ASSERT_EQ(es_reset(converted.get(),&e),ES_OK);auto fresh=instance(R,frequency);EXPECT_EQ(render(converted.get(),44101,1),render(fresh.get(),44101,503));
    }
}
TEST(Rate, PublicFaultPrefixKeepsTheLastCoherent48000Snapshot) {
    auto fault=instance(R,96000),reference=instance(R,96000);es_controls c{sizeof(c),ES_ABI_V2,1,1,.3,0,0};es_error e{};
    ASSERT_EQ(es_set_controls(fault.get(),&c,&e),ES_OK);ASSERT_EQ(es_set_controls(reference.get(),&c,&e),ES_OK);const auto expectedPcm=render(reference.get(),2);const auto coherent=telemetry(reference.get());
    unsigned ticks=0;RuntimeTestAccess::pull(*fault).setTickObserver([&](const Simulator &){if(++ticks==5)throw SimulationFault("injected48000 tick failure");});
    std::array<float,66> pcm;pcm.fill(42);std::uint32_t written=99;
    EXPECT_EQ(es_render(fault.get(),64,pcm.data()+1,64,&written,&e),ES_RENDER_FAULT);EXPECT_EQ(written,2u);EXPECT_TRUE(std::equal(expectedPcm.begin(),expectedPcm.end(),pcm.begin()+1));EXPECT_EQ(pcm[0],42);
    EXPECT_TRUE(std::all_of(pcm.begin()+3,pcm.end(),[](auto x){return x==42;}));const auto t=telemetry(fault.get());EXPECT_EQ(t.frames,2u);EXPECT_EQ(t.ticks,4u);EXPECT_EQ(t.native_frames,2u);EXPECT_EQ(t.state,ES_FAULTED);EXPECT_DOUBLE_EQ(t.crank_rad_s,coherent.crank_rad_s);
}
}
