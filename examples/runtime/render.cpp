#include <engine_sim/runtime.h>
#ifdef ENGINE_SIM_RENDER_LEGACY_TEST
#include "legacy_runtime.hpp"
#endif
#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
namespace fs=std::filesystem;
constexpr std::uint64_t MaxWaveFrames=(std::uint64_t{UINT32_MAX}-50)/4;
struct Event {std::uint64_t frame;es_controls controls;};
[[noreturn]] void fail(const std::string &text) {throw std::runtime_error(text);}
void utf8(const std::string &text) {
    for(std::size_t i=0;i<text.size();) {
        const auto c=static_cast<unsigned char>(text[i]);unsigned length=c<0x80?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
        if(!length||c==0||i+length>text.size())fail("invalid UTF-8 input");
        for(unsigned k=1;k<length;++k)if(static_cast<unsigned char>(text[i+k])<0x80||static_cast<unsigned char>(text[i+k])>0xbf)fail("invalid UTF-8 input");
        if(length>=3){const auto second=static_cast<unsigned char>(text[i+1]);if((c==0xe0&&second<0xa0)||(c==0xed&&second>0x9f)||(c==0xf0&&second<0x90)||(c==0xf4&&second>0x8f))fail("invalid UTF-8 input");}i+=length;
    }
}
std::uint64_t integer(const std::string &text,const char *field) {
    std::uint64_t value=0;const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if(text.empty()||parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size())fail(std::string(field)+": expected an unsigned decimal integer");return value;
}
double number(const std::string &text) {
    std::istringstream input(text);input.imbue(std::locale::classic());double value=0;input>>std::noskipws>>value;
    if(!input||input.peek()!=std::char_traits<char>::eof()||!std::isfinite(value))fail("schedule: expected a finite number");return value;
}
void validate(const es_controls &c,std::uint32_t mode,double limit) {
    if(c.ignition>1||c.starter>1||c.demand<0||c.demand>1||c.load_nm<0||c.load_nm>10000||c.driven_rpm<0||c.driven_rpm>12000)fail("schedule: control outside its valid range");
    if((mode==ES_FREE&&c.driven_rpm!=0)||(mode==ES_DRIVEN&&c.starter!=0))fail("schedule: controls incompatible with selected mode");
    if(mode==ES_DRIVEN&&c.driven_rpm>limit)fail("schedule: driven speed exceeds loaded engine limit");
}
std::vector<Event> schedule(const fs::path &path,std::uint64_t frames,std::uint32_t mode) {
    std::ifstream file(path,std::ios::binary);if(!file)fail("schedule: cannot open file");
    std::string line;std::vector<Event> events;std::uint64_t previous=0;
    if(!std::getline(file,line))fail("schedule: missing header");utf8(line);if(line.compare(0,3,"\xef\xbb\xbf")==0)line.erase(0,3);if(!line.empty()&&line.back()=='\r')line.pop_back();
    if(line!="frame\tdemand\tload_nm\tignition\tstarter\tdriven_rpm")fail("schedule: invalid TSV header");
    while(std::getline(file,line)) {
        utf8(line);if(!line.empty()&&line.back()=='\r')line.pop_back();std::array<std::string,6> fields;std::size_t start=0;
        for(unsigned i=0;i<6;++i) {const auto tab=line.find('\t',start);if((i<5&&tab==std::string::npos)||(i==5&&tab!=std::string::npos))fail("schedule: expected six tab-separated fields");fields[i]=line.substr(start,tab==std::string::npos?tab:tab-start);start=tab==std::string::npos?line.size():tab+1;}
        Event e{};e.frame=integer(fields[0],"schedule.frame");if(e.frame>=frames||(!events.empty()&&e.frame<previous))fail("schedule: frames must be nondecreasing and within output interval");previous=e.frame;
        const auto ignition=integer(fields[3],"schedule.ignition"),starter=integer(fields[4],"schedule.starter");if(ignition>1||starter>1)fail("schedule: booleans must be zero or one");
        e.controls={sizeof(es_controls),ES_ABI_V2,static_cast<std::uint32_t>(ignition),static_cast<std::uint32_t>(starter),number(fields[1]),number(fields[2]),number(fields[5])};validate(e.controls,mode,12000);events.push_back(e);
    }
    if(!file.eof())fail("schedule: read failed");return events;
}
class TemporaryWave {
public:
    explicit TemporaryWave(fs::path target):m_target(std::move(target)) {
        if(fs::exists(fs::symlink_status(m_target)))fail("output already exists");
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for(unsigned attempt=0;attempt<100&&!m_file;++attempt) {
            m_temp=m_target;m_temp+=".tmp-"+std::to_string(stamp)+"-"+std::to_string(attempt);
#ifdef _WIN32
            HANDLE handle=CreateFileW(m_temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(handle==INVALID_HANDLE_VALUE){if(GetLastError()==ERROR_FILE_EXISTS||GetLastError()==ERROR_ALREADY_EXISTS)continue;fail("output: cannot create temporary file");}
            const int fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_WRONLY|_O_BINARY);if(fd<0){CloseHandle(handle);std::error_code ignored;fs::remove(m_temp,ignored);fail("output: cannot open temporary stream");}m_file=_fdopen(fd,"wb");if(!m_file)_close(fd);
#else
            const int fd=::open(m_temp.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0){if(errno==EEXIST)continue;fail("output: cannot create temporary file");}m_file=fdopen(fd,"wb");if(!m_file)::close(fd);
#endif
            if(!m_file){std::error_code ignored;fs::remove(m_temp,ignored);fail("output: cannot open temporary stream");}
        }
        if(!m_file)fail("output: no unique temporary filename available");
    }
    ~TemporaryWave(){if(m_file)std::fclose(m_file);if(!m_done){std::error_code ignored;fs::remove(m_temp,ignored);}}
    TemporaryWave(const TemporaryWave &)=delete;
    TemporaryWave &operator=(const TemporaryWave &)=delete;
    void bytes(const void *data,std::size_t size){if(std::fwrite(data,1,size,m_file)!=size)fail("output: write failed");}
    void u16(std::uint16_t n){const unsigned char v[]{static_cast<unsigned char>(n),static_cast<unsigned char>(n>>8)};bytes(v,2);}
    void u32(std::uint32_t n){const unsigned char v[]{static_cast<unsigned char>(n),static_cast<unsigned char>(n>>8),static_cast<unsigned char>(n>>16),static_cast<unsigned char>(n>>24)};bytes(v,4);}
    void finish() {
        const int flushed = std::fflush(m_file), closed = std::fclose(m_file);
        m_file = nullptr;
        if (flushed || closed) fail("output: close failed");
#ifdef _WIN32
        if (!MoveFileW(m_temp.c_str(), m_target.c_str())) {
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS)
                fail("output appeared before publication");
            fail("output: cannot publish completed WAV");
        }
#else
        // Linking the closed file publishes it atomically and leaves an existing name intact.
        std::error_code error;
        fs::create_hard_link(m_temp, m_target, error);
        if (error == std::errc::file_exists) fail("output appeared before publication");
        if (error) fail("output: cannot publish completed WAV: " + error.message());
        fs::remove(m_temp, error);
#endif
        m_done = true;
    }
private:
    fs::path m_target,m_temp;std::FILE *m_file=nullptr;bool m_done=false;
};
void render(const std::vector<std::string> &args) {
    static_assert(sizeof(float)==4&&std::numeric_limits<float>::is_iec559,"IEEE float32 required");
#ifndef ENGINE_SIM_RENDER_LEGACY_TEST
    const std::array<std::string,8> keys{"--entry","--asset-root","--rate","--mode","--seed","--frames","--schedule","--out"};
    constexpr auto help = "engine-sim-render --entry JSON --asset-root PATH --rate 44100|48000 --mode free|driven --seed U64 --frames U64 --schedule TSV --out WAV\n";
    constexpr auto arity = "expected all eight options; use --help";
    for (std::size_t i = 0; i < args.size(); i += 2) {
        if (args[i] == "--library-root")
            fail("--library-root was removed; convert the legacy engine to JSON with engine-sim-convert");
    }
#else
    const std::array<std::string,9> keys{"--entry","--library-root","--asset-root","--rate","--mode","--seed","--frames","--schedule","--out"};
    constexpr auto help = "engine-sim-render-legacy-test --entry PATH --library-root PATH --asset-root PATH --rate 44100|48000 --mode free|driven --seed U64 --frames U64 --schedule TSV --out WAV\n";
    constexpr auto arity = "expected all nine options; use --help";
#endif
    std::map<std::string,std::string> options;
    if(args.size()==1&&args[0]=="--help"){std::cout<<help;return;}
    if(args.size()!=2*keys.size())fail(arity);
    for(std::size_t i=0;i<args.size();i+=2){utf8(args[i]);utf8(args[i+1]);if(std::find(keys.begin(),keys.end(),args[i])==keys.end()||!options.emplace(args[i],args[i+1]).second)fail("unknown or duplicate option");}
    const auto frames=integer(options.at("--frames"),"frames");if(frames>MaxWaveFrames)fail("frames: exceeds 32-bit RIFF capacity");
    const auto rate=integer(options.at("--rate"),"rate");if(rate!=44100&&rate!=48000)fail("rate: expected 44100 or 48000");
    const auto mode=options.at("--mode")=="free"?ES_FREE:options.at("--mode")=="driven"?ES_DRIVEN:2u;if(mode>ES_DRIVEN)fail("mode: expected free or driven");
    auto events=schedule(fs::u8path(options.at("--schedule")),frames,mode);
    es_config config{};es_error error{};const auto check=[&](es_status s){if(s!=ES_OK)fail(error.message);};
    check(es_config_init(ES_ABI_V2,sizeof(config),&config,&error));config.output_hz=static_cast<std::uint32_t>(rate);config.mode=mode;config.seed=integer(options.at("--seed"),"seed");
    es_runtime *raw=nullptr;check(es_create(&config,&raw,&error));std::unique_ptr<es_runtime,decltype(&es_destroy)> engine(raw,es_destroy);
#ifndef ENGINE_SIM_RENDER_LEGACY_TEST
    es_diagnostic detail{};
    detail.size = sizeof(detail);
    detail.abi = ES_ABI_V2;
    if (es_load_json_file(raw, options.at("--entry").c_str(), options.at("--asset-root").c_str(),
                          &detail, &error) != ES_OK)
        fail(std::string(detail.code) + ": " + error.message);
#else
    check(engine_sim::legacy_runtime::load(raw,options.at("--entry").c_str(),options.at("--library-root").c_str(),options.at("--asset-root").c_str(),&error));
#endif
    es_telemetry state{};state.size=sizeof(state);state.abi=ES_ABI_V2;check(es_get_telemetry(raw,&state,&error));for(const auto &e:events)validate(e.controls,mode,state.driven_limit_rpm);
    // Validate overwritten rows too, then retain only the final full vector.
    std::vector<Event> timeline;for(const auto &e:events){if(!timeline.empty()&&timeline.back().frame==e.frame)timeline.back()=e;else timeline.push_back(e);}
    TemporaryWave wave(fs::u8path(options.at("--out")));wave.bytes("RIFF",4);wave.u32(static_cast<std::uint32_t>(50+4*frames));wave.bytes("WAVEfmt ",8);wave.u32(18);wave.u16(3);wave.u16(1);wave.u32(config.output_hz);wave.u32(config.output_hz*4);wave.u16(4);wave.u16(32);wave.u16(0);wave.bytes("fact",4);wave.u32(4);wave.u32(static_cast<std::uint32_t>(frames));wave.bytes("data",4);wave.u32(static_cast<std::uint32_t>(frames*4));
    std::array<float,4096> pcm;std::array<unsigned char,4096*4> bytes;std::size_t event=0;
    for(std::uint64_t pos=0;pos<frames;){if(event<timeline.size()&&timeline[event].frame==pos){check(es_set_controls(raw,&timeline[event].controls,&error));++event;}
        const auto end=event<timeline.size()?timeline[event].frame:frames;const auto count=static_cast<std::uint32_t>(std::min<std::uint64_t>(pcm.size(),end-pos));std::uint32_t written=0;check(es_render(raw,count,pcm.data(),static_cast<std::uint32_t>(pcm.size()),&written,&error));if(written!=count)fail("runtime: incomplete successful render");
        for(std::size_t i=0;i<count;++i){std::uint32_t word;std::memcpy(&word,&pcm[i],4);for(unsigned b=0;b<4;++b)bytes[4*i+b]=static_cast<unsigned char>(word>>(b*8));}wave.bytes(bytes.data(),4*count);pos+=count;
    }
    check(es_get_telemetry(raw,&state,&error));wave.finish();std::cout<<state.frames<<" frames at "<<rate<<" Hz; "<<state.measured_rpm<<" RPM\n";
}
int run(const std::vector<std::string> &args){try{render(args);return 0;}catch(const std::exception &e){std::cerr<<"engine-sim-render: "<<e.what()<<'\n';return 1;}}
}
#ifdef _WIN32
int wmain(int argc,wchar_t **argv){try{std::vector<std::string> args;for(int i=1;i<argc;++i){const int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);if(n<=0)fail("arguments: invalid UTF-16");std::string text(n,'\0');if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,text.data(),n,nullptr,nullptr))fail("arguments: UTF-8 conversion failed");text.resize(n-1);args.push_back(std::move(text));}return run(args);}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
#else
int main(int argc,char **argv){try{return run(std::vector<std::string>(argv+1,argv+argc));}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
#endif
