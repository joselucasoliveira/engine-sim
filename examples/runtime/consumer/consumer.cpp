#include <engine_sim/runtime.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

static int run(const std::vector<std::string> &paths) {
    if(paths.size()!=2)return 2;
    try {
        es_error error{};const auto check=[&](es_status s){if(s)throw std::runtime_error(error.message);};
        es_config config{};check(es_config_init(ES_ABI_V2,sizeof(config),&config,&error));config.output_hz=48000;
        es_runtime *raw=nullptr;check(es_create(&config,&raw,&error));std::unique_ptr<es_runtime,decltype(&es_destroy)> rt(raw,es_destroy);
        check(es_load_json_file(rt.get(),paths[0].c_str(),paths[1].c_str(),nullptr,&error));
        es_controls c{sizeof(c),ES_ABI_V2,1,1,.25,0,0};check(es_set_controls(rt.get(),&c,&error));
        float block[257];double energy=0;
        for(unsigned pos=0;pos<96000;) {
            if(pos==48000){c.starter=0;check(es_set_controls(rt.get(),&c,&error));}
            const auto count=std::min({257u,96000-pos,pos<48000?48000-pos:96000-pos});unsigned written=0;
            check(es_render(rt.get(),count,block,257,&written,&error));if(written!=count)throw std::runtime_error("short render");
            for(unsigned i=0;i<written;++i){if(!std::isfinite(block[i]))throw std::runtime_error("nonfinite PCM");energy+=static_cast<double>(block[i])*block[i];}pos+=written;
        }
        es_telemetry t{};t.size=sizeof(t);t.abi=ES_ABI_V2;check(es_get_telemetry(rt.get(),&t,&error));
        if(t.frames!=96000||t.native_frames!=88200||t.ticks!=20000||t.measured_rpm<100||energy<=0)throw std::runtime_error("render invariant failed");
        check(es_reset(rt.get(),&error));check(es_unload(rt.get(),&error));check(es_unload(rt.get(),&error));
        std::printf("C++: frames=%llu native=%llu rpm=%.3f energy=%.9g\n",static_cast<unsigned long long>(t.frames),static_cast<unsigned long long>(t.native_frames),t.measured_rpm,energy);return 0;
    }catch(const std::exception &e){std::fprintf(stderr,"C++ consumer: %s\n",e.what());return 1;}
}
#ifdef _WIN32
int wmain(int argc,wchar_t **argv){std::vector<std::string> paths;for(int i=1;i<argc;++i)paths.push_back(std::filesystem::path(argv[i]).u8string());return run(paths);}
#else
int main(int argc,char **argv){std::vector<std::string> paths;for(int i=1;i<argc;++i)paths.emplace_back(argv[i]);return run(paths);}
#endif
