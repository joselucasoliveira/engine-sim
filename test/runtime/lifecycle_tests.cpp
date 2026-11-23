#include "runtime.hpp"
#include "simulation_fault.h"
#include <gtest/gtest.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace engine_sim {
struct RuntimeTestAccess {
    static PullEngine &pull(es_runtime &rt) { return *rt.implementation.m_session->pull; }
};
}
namespace {
namespace fs=std::filesystem;
using Handle=std::unique_ptr<es_runtime,decltype(&es_destroy)>;
void checked(es_status status,const es_error &error) { if(status!=ES_OK) throw std::runtime_error(error.message); }
std::string read(const fs::path &p) {
    std::ifstream f(p,std::ios::binary);if(!f) throw std::runtime_error("test file missing");
    std::string text(static_cast<std::size_t>(fs::file_size(p)), '\0');
    if(!f.read(text.data(),static_cast<std::streamsize>(text.size()))) throw std::runtime_error("test file read failed");
    return text;
}
void write(const fs::path &p,const std::string &s) { std::ofstream f(p,std::ios::binary);f.write(s.data(),s.size());if(!f) throw std::runtime_error("test file write failed"); }
void replace(std::string &s,const std::string &from,const std::string &to) { const auto at=s.find(from);if(at==std::string::npos) throw std::runtime_error("fixture replacement absent");s.replace(at,from.size(),to); }
es_telemetry telemetry(es_runtime *rt) { es_error error{};es_telemetry t{};t.size=sizeof(t);t.abi=ES_ABI_V2;checked(es_get_telemetry(rt,&t,&error),error);return t; }
void controls(es_runtime *rt,double demand=.3,bool starter=true) { es_error error{};const es_controls c{sizeof(c),ES_ABI_V2,1,static_cast<std::uint32_t>(starter),demand,0,0};checked(es_set_controls(rt,&c,&error),error); }
std::vector<float> render(es_runtime *rt,std::size_t frames,std::size_t part=257) {
    es_error error{};std::vector<float> out(frames);
    for(std::size_t n=0;n<frames;) { const auto count=static_cast<std::uint32_t>(std::min(part,frames-n));std::uint32_t written=99;checked(es_render(rt,count,out.data()+n,count,&written,&error),error);if(written!=count) throw std::runtime_error("short successful render");n+=count; }
    return out;
}
struct Result {
    std::vector<float> pcm;
    es_telemetry last;
    bool operator==(const Result &r) const { return pcm==r.pcm && std::memcmp(&last,&r.last,sizeof(last))==0; }
};
Result timeline(es_runtime *rt,std::size_t part=257) {
    const std::array<std::size_t,7> edges{0,1,147,257,441,44100,88201};Result result;
    for(std::size_t i=0;i+1<edges.size();++i) {
        controls(rt,i%2?.35:.2,i<5);auto pcm=render(rt,edges[i+1]-edges[i],part);result.pcm.insert(result.pcm.end(),pcm.begin(),pcm.end());
    }
    result.last=telemetry(rt);return result;
}
class RuntimeLifetime:public testing::Test {
protected:
    std::array<fs::path,2> roots;
    void SetUp() override {
        const auto *source=std::getenv("ENGINE_SIM_TEST_SOURCE");ASSERT_NE(source,nullptr);
        const auto base=fs::current_path()/fs::u8path(std::string(testing::UnitTest::GetInstance()->current_test_info()->name())+u8" espaço Ω");
        for(unsigned i=0;i<2;++i) {
            roots[i]=base/std::to_string(i);fs::create_directories(roots[i]);
            const auto fixture = fs::u8path(source) / "examples/runtime/fixture";
            for (const auto *file : {"main.json", "impulse.wav"})
                fs::copy_file(fixture / file, roots[i] / file, fs::copy_options::overwrite_existing);
            auto text=read(roots[i]/"main.json");replace(text,"\"jitter\":0.0","\"jitter\":0.2"); replace(text, "\"noise\":0.0", "\"noise\":0.25");replace(text,"\"burning_efficiency_randomness\":0.0","\"burning_efficiency_randomness\":0.15");
            if(i) { replace(text,"\"throw\":0.04","\"throw\":0.035");replace(text,"\"simulation_frequency\":10000.0","\"simulation_frequency\":20000.0"); }
            write(roots[i]/"main.json",text);
        }
    }
    Handle prepare(unsigned index=0,std::uint64_t seed=17,std::uint32_t hz=0,double torque=0) {
        es_error error{};es_config c{};checked(es_config_init(ES_ABI_V2,sizeof(c),&c,&error),error);c.seed=seed;c.simulation_hz=hz;c.starter_torque_nm=torque;
        es_runtime *p=nullptr;checked(es_create(&c,&p,&error),error);Handle rt(p,es_destroy);
        auto entry=(roots[index]/"main.json").u8string(),assets=roots[index].u8string();
        checked(es_load_json_file(p,entry.c_str(),assets.c_str(),nullptr,&error),error);
        // The runtime must not retain any of these borrowed string buffers.
        std::fill(entry.begin(),entry.end(),'!');std::fill(assets.begin(),assets.end(),'!');
        return rt;
    }
};
TEST_F(RuntimeLifetime, ResetReproducesFreshPcmPhaseDefaultControlsAndOriginalConfig) {
    for(const auto hz:{0u,4000u,96000u}) {
        auto used=prepare(0,UINT64_MAX,hz,25),fresh=prepare(0,UINT64_MAX,hz,25);
        const auto initial=telemetry(fresh.get());const auto expected=timeline(fresh.get(),503);
        controls(used.get(),.7);render(used.get(),19007);es_error error{};checked(es_reset(used.get(),&error),error);
        const auto reset=telemetry(used.get());EXPECT_EQ(std::memcmp(&initial,&reset,sizeof(reset)),0);
        EXPECT_EQ(timeline(used.get(),7),expected);
        auto defaultReference=prepare(0,UINT64_MAX,hz,25);checked(es_reset(used.get(),&error),error);
        EXPECT_EQ(render(used.get(),4410),render(defaultReference.get(),4410));
    }
}
TEST_F(RuntimeLifetime, ChangedMalformedAndRemovedInputsRejectBeforeParsingAndKeepContinuation) {
    for(const auto relative:{"main.json","impulse.wav"}) {
        const auto path=roots[0]/relative;const auto original=read(path);
        for(int mode=0;mode<3;++mode) {
            auto active=prepare(),reference=prepare();controls(active.get());controls(reference.get());
            EXPECT_EQ(render(active.get(),1231),render(reference.get(),1231));
            controls(active.get(),.4,false);controls(reference.get(),.4,false);const auto before=telemetry(active.get());
            if(mode==0) write(path,original+"\n// changed bytes\n");
            if(mode==1) write(path,std::string("\xff\xc0",2));
            if(mode==2) fs::remove(path);
            es_error error{};EXPECT_EQ(es_reset(active.get(),&error),ES_RESOURCE)<<error.message;
            const auto after=telemetry(active.get());EXPECT_EQ(std::memcmp(&before,&after,sizeof(before)),0);
            EXPECT_EQ(render(active.get(),4410),render(reference.get(),4410));
            write(path,original);checked(es_reset(active.get(),&error),error);EXPECT_EQ(telemetry(active.get()).frames,0u);
        }
    }
}
TEST_F(RuntimeLifetime, ExplicitLoadAcceptsModifiedInputsAndResetBindsTheNewIdentity) {
    auto active=prepare();const auto path=roots[0]/"main.json";auto text=read(path);replace(text,"\"noise\":0.25","\"noise\":0.45");write(path,text);
    es_error error{};EXPECT_EQ(es_reset(active.get(),&error),ES_RESOURCE);
    checked(es_load_json_file(active.get(),path.u8string().c_str(),roots[0].u8string().c_str(),nullptr,&error),error);
    auto reference=prepare();EXPECT_EQ(timeline(active.get()),timeline(reference.get()));
    checked(es_reset(active.get(),&error),error);auto fresh=prepare();EXPECT_EQ(timeline(active.get()),timeline(fresh.get()));
}
TEST_F(RuntimeLifetime, ResolvedPathChangesAreRejectedEvenWhenBytesAreIdentical) {
    const auto alias=roots[0]/"entry alias.json",alternate=roots[0]/"alternate.json";write(alternate,read(roots[0]/"main.json"));
    // Retire only this test's prior alias when the same build is tested again.
    fs::remove(alias);
    std::error_code failure;fs::create_symlink(roots[0]/"main.json",alias,failure);
#ifdef _WIN32
    if(failure==std::errc::permission_denied || failure==std::errc::operation_not_supported)
        GTEST_SKIP()<<"Native symlink creation unavailable: "<<failure.message();
#endif
    ASSERT_FALSE(failure)<<failure.message();
    auto active=prepare();es_error error{};
    checked(es_load_json_file(active.get(),alias.u8string().c_str(),roots[0].u8string().c_str(),nullptr,&error),error);
    controls(active.get());render(active.get(),441);const auto before=telemetry(active.get());
    fs::remove(alias);fs::create_symlink(alternate,alias);
    EXPECT_EQ(es_reset(active.get(),&error),ES_RESOURCE)<<error.message;const auto after=telemetry(active.get());EXPECT_EQ(std::memcmp(&before,&after,sizeof(before)),0);
    fs::remove(alias);fs::create_symlink(roots[0]/"main.json",alias);checked(es_reset(active.get(),&error),error);EXPECT_EQ(telemetry(active.get()).frames,0u);
}
TEST_F(RuntimeLifetime, RepeatedFaultResetUnloadAndReloadRetireEverySession) {
    es_error error{};EXPECT_EQ(es_reset(nullptr,&error),ES_INVALID);
    for(int repeat=0;repeat<12;++repeat) {
        auto active=prepare(repeat%2,repeat);controls(active.get());render(active.get(),257);
        engine_sim::RuntimeTestAccess::pull(*active).setTickObserver([](const Simulator &){throw engine_sim::SimulationFault("lifecycle fault");});
        std::array<float,64> pcm;pcm.fill(42);std::uint32_t written=99;
        EXPECT_EQ(es_render(active.get(),64,pcm.data(),64,&written,&error),ES_RENDER_FAULT);
        EXPECT_EQ(telemetry(active.get()).state,ES_FAULTED);
        const auto path=roots[repeat%2]/"main.json";const auto original=read(path);write(path,"broken (");
        EXPECT_EQ(es_reset(active.get(),&error),ES_RESOURCE);EXPECT_EQ(telemetry(active.get()).state,ES_FAULTED);
        write(path,original);checked(es_reset(active.get(),&error),error);EXPECT_EQ(telemetry(active.get()).frames,0u);
        auto reference=prepare(repeat%2,repeat);EXPECT_EQ(timeline(active.get(),503),timeline(reference.get(),31));
        checked(es_unload(active.get(),&error),error);checked(es_unload(active.get(),&error),error);EXPECT_EQ(es_reset(active.get(),&error),ES_STATE);
        checked(es_load_json_file(active.get(),path.u8string().c_str(),roots[repeat%2].u8string().c_str(),nullptr,&error),error);
        EXPECT_EQ(telemetry(active.get()).state,ES_READY);
    }
}
TEST_F(RuntimeLifetime, InterleavedAndConcurrentDistinctHandlesMatchIsolatedTimelines) {
    EXPECT_NE(es_capabilities() & ES_CAP_PARALLEL_RENDER,0u);
    auto isolatedA=prepare(0,17),isolatedB=prepare(1,UINT64_MAX);
    const auto expectedA=timeline(isolatedA.get()),expectedB=timeline(isolatedB.get());
    auto otherSeed=prepare(0,18);EXPECT_NE(timeline(otherSeed.get()).pcm,expectedA.pcm);EXPECT_NE(expectedA.pcm,expectedB.pcm);
    auto a=prepare(0,17),b=prepare(1,UINT64_MAX);
    std::mutex mutex;std::condition_variable ready;unsigned entered=0;std::atomic<bool> timeout{false};bool firstA=true,firstB=true;
    const auto rendezvous=[&](bool &first) { if(!first)return;first=false;std::unique_lock<std::mutex> lock(mutex);++entered;ready.notify_all();if(!ready.wait_for(lock,std::chrono::seconds(10),[&]{return entered==2;})) timeout=true; };
    engine_sim::RuntimeTestAccess::pull(*a).setTickObserver([&](const Simulator &){rendezvous(firstA);});
    engine_sim::RuntimeTestAccess::pull(*b).setTickObserver([&](const Simulator &){rendezvous(firstB);});
    auto asyncA=std::async(std::launch::async,[&]{return timeline(a.get(),1);});
    auto asyncB=std::async(std::launch::async,[&]{return timeline(b.get(),503);});
    EXPECT_EQ(asyncA.get(),expectedA);EXPECT_EQ(asyncB.get(),expectedB);EXPECT_FALSE(timeout.load());EXPECT_EQ(entered,2u);
    es_error error{};checked(es_reset(a.get(),&error),error);checked(es_reset(b.get(),&error),error);
    engine_sim::RuntimeTestAccess::pull(*a).setTickObserver({});engine_sim::RuntimeTestAccess::pull(*b).setTickObserver({});
    Result interleavedA,interleavedB;
    for(std::size_t n=0;n<88201;) {
        const std::array<std::size_t,7> edges{0,1,147,257,441,44100,88201};
        auto edge=std::upper_bound(edges.begin(),edges.end(),n)-edges.begin()-1;
        const auto count=std::min<std::size_t>(503,edges[edge+1]-n);
        controls(a.get(),edge%2?.35:.2,edge<5);controls(b.get(),edge%2?.35:.2,edge<5);
        auto pa=render(a.get(),count),pb=render(b.get(),count);interleavedA.pcm.insert(interleavedA.pcm.end(),pa.begin(),pa.end());interleavedB.pcm.insert(interleavedB.pcm.end(),pb.begin(),pb.end());n+=count;
    }
    interleavedA.last=telemetry(a.get());interleavedB.last=telemetry(b.get());EXPECT_EQ(interleavedA,expectedA);EXPECT_EQ(interleavedB,expectedB);
}
TEST_F(RuntimeLifetime, IndependentPreparationAndDestructionDoNotHoldTheRenderOwner) {
    auto reference=prepare(1,77);const auto expected=timeline(reference.get());auto active=prepare(1,77);auto victim=prepare();
    std::mutex mutex;std::condition_variable change;bool atTick=false,finished=false,timeout=false;
    engine_sim::RuntimeTestAccess::pull(*active).setTickObserver([&](const Simulator &){
        if(atTick) return;std::unique_lock<std::mutex> lock(mutex);atTick=true;change.notify_all();
        if(!change.wait_for(lock,std::chrono::seconds(10),[&]{return finished;})) timeout=true;
    });
    auto future=std::async(std::launch::async,[&]{return timeline(active.get());});
    {std::unique_lock<std::mutex> lock(mutex);ASSERT_TRUE(change.wait_for(lock,std::chrono::seconds(10),[&]{return atTick;}));}
    victim.reset();
    for(int i=0;i<6;++i) { auto unrelated=prepare(0,100+i);controls(unrelated.get());render(unrelated.get(),257); }
    {std::lock_guard<std::mutex> lock(mutex);finished=true;}change.notify_all();EXPECT_EQ(future.get(),expected);EXPECT_FALSE(timeout);
}
TEST_F(RuntimeLifetime, OneHandleMovesBetweenOwnersOnlyAtSynchronizedBoundaries) {
    auto active=prepare(),reference=prepare();controls(active.get());controls(reference.get());
    auto task=std::async(std::launch::async,[&]{return render(active.get(),5003,7);});const auto first=task.get();
    EXPECT_EQ(first,render(reference.get(),5003));EXPECT_EQ(render(active.get(),8009,503),render(reference.get(),8009,1));
    es_error error{};checked(es_reset(active.get(),&error),error);auto fresh=prepare();EXPECT_EQ(timeline(active.get()),timeline(fresh.get()));
}
}
