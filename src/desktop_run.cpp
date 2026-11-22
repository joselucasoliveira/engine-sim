#include "desktop_run.h"
#include "engine_sim_application.h"
#include "units.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

namespace desktop {
namespace {
namespace fs = std::filesystem;
std::string jsonString(const std::string &value) {
    std::ostringstream out; out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c) << std::dec;
        else out << c;
    }
    out << '"'; return out.str();
}
bool within(const fs::path &path, const fs::path &root) {
    const auto relative = path.lexically_relative(root);
    return path == root || (!relative.empty() && *relative.begin() != "..");
}
void protectOutput(const fs::path &file, const fs::path &resources = {}) {
    if (!file.is_absolute() || !fs::is_directory(file.parent_path()) || fs::exists(file) || fs::is_symlink(file))
        throw DesktopError("report", "requires a new absolute file in an existing writable directory");
    const auto resolved = fs::weakly_canonical(file);
    if (!resources.empty() && within(resolved, fs::weakly_canonical(resources)))
        throw DesktopError("report", "output must be outside the resource tree");
    for (auto parent = resolved.parent_path(); !parent.empty(); parent = parent.parent_path()) {
        if (fs::exists(parent / ".git") && fs::exists(parent / "include/engine.h"))
            throw DesktopError("report", "output must be outside the source checkout");
        if (parent == parent.parent_path()) break;
    }
}
std::uint64_t fileDigest(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw DesktopError("script", "cannot read " + path.u8string());
    std::uint64_t result = 14695981039346656037ULL;
    std::array<char,4096> block{}; std::size_t size = 0;
    while (file.read(block.data(),block.size()) || file.gcount()) {
        size += static_cast<std::size_t>(file.gcount());
        if (size > 4 * 1024 * 1024) throw DesktopError("script", "smoke entry exceeds four MiB");
        for (std::streamsize i=0; i<file.gcount(); ++i) { result ^= static_cast<unsigned char>(block[i]); result *= 1099511628211ULL; }
    }
    if (!file.eof()) throw DesktopError("script", "failed while reading " + path.u8string());
    return result;
}
class ObservedApplication final : public EngineSimApplication {
public:
    explicit ObservedApplication(std::shared_ptr<RunEvidence> evidence) : evidence(std::move(evidence)) {}
    void pauseForSmoke(unsigned frame) {
        const auto first = evidence->options.frames * 3 / 4;
        // The inherited interactive controls expose single-step but no pause toggle.
        // Only this explicit smoke fixture sets the existing pause state.
        m_paused = frame >= first && frame < first + 3;
    }
private:
    void process(float seconds) override {
        if (evidence) {
            if (previous && previous!=viewGeneration()) ++evidence->acceptedReloads;
            previous=viewGeneration();
        }
        EngineSimApplication::process(seconds);
        if (!evidence) return;
        ++evidence->processCalls;
        evidence->simulationSteps += getSimulator()->getFrameIterationCount();
        auto *engine = getSimulator()->getEngine(); evidence->cylinders = engine->getCylinderCount();
        const double rpm = engine->getRpm();
        if (!std::isfinite(rpm)) throw DesktopError("smoke simulation", "nonfinite engine speed");
        evidence->peakRpm = std::max(evidence->peakRpm,rpm);
        if (m_targetSpeedSetting==1.0) evidence->throttleChanged=true;
    }
    std::shared_ptr<RunEvidence> evidence;
    std::uint64_t previous=0;
};
}
RunOptions parseRunOptions(const std::vector<std::string> &arguments) {
    RunOptions options; std::set<std::string> seen;
    for (std::size_t i=1; i<arguments.size(); ++i) {
        const auto name = arguments[i];
        if (!seen.insert(name).second) throw DesktopError("arguments", "duplicate " + name);
        if (name == "--help") { options.help = true; continue; }
        if (name == "--smoke") { options.smoke = true; continue; }
        if (name != "--frames" && name != "--report" && name != "--audio-mode" && name != "--resources" && name != "--script")
            throw DesktopError("arguments", "unknown option " + name);
        if (++i == arguments.size() || arguments[i].empty()) throw DesktopError("arguments", name + " requires a value");
        const auto &value = arguments[i];
        if (name == "--frames") {
            if (value.find_first_not_of("0123456789") != std::string::npos || value.size()>5)
                throw DesktopError("arguments", "frames must be an integer between 60 and 10000");
            options.frames = static_cast<unsigned>(std::stoul(value));
            if (options.frames<60 || options.frames>10000) throw DesktopError("arguments", "frames must be between 60 and 10000");
        } else if (name == "--report") options.report = fs::u8path(value);
        else if (name == "--resources") options.resources.resources = fs::u8path(value);
        else if (name == "--script") options.resources.script = fs::u8path(value);
        else if (value == "virtual") options.virtualAudio = true;
        else if (value != "device") throw DesktopError("arguments", "audio mode must be device or virtual");
    }
    if (options.help) return options;
    if (options.smoke) {
        protectOutput(options.report,options.resources.resources);
        if (!options.resources.script.empty() && !options.resources.script.is_absolute())
            throw DesktopError("arguments", "smoke script must be an absolute path");
    } else if (!options.report.empty() || seen.count("--frames")) throw DesktopError("arguments", "report and frames require --smoke");
    return options;
}
void RunEvidence::beforeFrame(Host &host) {
    if (frames == 0) start = std::chrono::steady_clock::now();
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120))
        throw DesktopError("smoke timeout", "frame loop exceeded 120 seconds");
    if (frames >= options.frames) return;
    const auto n = static_cast<unsigned>(frames), count = options.frames;
    if (onFrame) onFrame(n);
    const auto key = [&](unsigned frame,Key value,bool down) { if (n==frame) host.queueKeyEvent(value,down); };
    key(0,Key::S,true); key(0,Key::A,true); key(1,Key::A,false);
    key(count/8,Key::R,true); key(count/6,Key::R,false);
    for (auto frame : {count/4,count/2}) { key(frame,Key::Tab,true); key(frame+1,Key::Tab,false); }
    key(count*5/8,Key::Return,true); key(count*5/8+1,Key::Return,false);
    key(count*5/8+2,Key::A,true); key(count*5/8+3,Key::A,false);
    key(count*3/4+1,Key::Right,true); key(count*3/4+2,Key::Right,false);
    key(count-1,Key::S,false);
    if (n==count/3) host.resizeForSmoke(1800,1000);
    if (n==count-4) host.resizeForSmoke(1920,1080);
}
bool RunEvidence::key(Key key,bool value,bool edge) {
    if (value) ++(edge ? edges : held).at(static_cast<std::size_t>(key));
    return value;
}
void RunEvidence::source(const AudioSegments &segments) {
    const auto record = [this](const std::int16_t *samples,AudioOffset count) {
        for (AudioOffset i=0;i<count;++i) {
            const auto value = static_cast<std::uint16_t>(samples[i]);
            sourceDigest ^= value & 255; sourceDigest *= 1099511628211ULL;
            sourceDigest ^= value >> 8; sourceDigest *= 1099511628211ULL;
            ++sourceFrames; if (samples[i]) ++nonzeroSource;
        }
    };
    record(segments.first,segments.firstCount); record(segments.second,segments.secondCount);
}
void RunEvidence::afterFrame(Host &host) {
    for (const auto &packet : host.drawPackets()) {
        if (packet.kind==DrawPacket::Kind::Model) ++modelPackets;
        else if (packet.kind==DrawPacket::Kind::Generated && packet.faceCount>0) ++generatedPackets;
        else if (packet.kind==DrawPacket::Kind::Text && !packet.text.empty()) ++textPackets;
    }
    if (frames>options.frames/3 && host.frame().window.gameWidth==1800 && host.frame().window.gameHeight==1000) resized=true;
    if (frames==1 || frames+1==options.frames) {
        const auto &w=host.frame().window;
        if (w.framebufferWidth<=0 || w.framebufferHeight<=0 || w.framebufferWidth>8192 || w.framebufferHeight>8192)
            throw DesktopError("smoke readback","invalid framebuffer size");
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w.framebufferWidth)*w.framebufferHeight*4);
        host.readScreen(pixels.data(),pixels.size());
        const auto image=options.report.parent_path()/(options.report.stem().u8string()+"-frame-"+std::to_string(frames)+".ppm");
        protectOutput(image,resourceRoot); std::ofstream out(image,std::ios::binary);
        out << "P6\n" << w.framebufferWidth << ' ' << w.framebufferHeight << "\n255\n";
        std::uint64_t different=0;
        for (std::size_t i=0;i<pixels.size();i+=4) {
            out.write(reinterpret_cast<const char *>(pixels.data()+i),3);
            if (pixels[i]!=pixels[0] || pixels[i+1]!=pixels[1] || pixels[i+2]!=pixels[2]) ++different;
        }
        if (!out) throw DesktopError("smoke readback","cannot write image");
        if (different<1000) throw DesktopError("smoke readback","presented image has no meaningful geometry or labels");
        coloredPixels += different; ++screenshots;
    }
    ++frames;
    // Only explicit smoke runs are paced; normal interactive timing is unchanged.
    std::this_thread::sleep_until(start+std::chrono::microseconds(frames*1000000/60));
}
void RunEvidence::validate() const {
    const auto edge = [&](Key key) { return edges[static_cast<std::size_t>(key)]; };
    if (frames!=options.frames || processCalls+2!=frames || !simulationSteps || cylinders<=0 || peakRpm<=0 || !modelPackets || !generatedPackets || !textPackets
        || screenshots!=2 || !coloredPixels || !resized || edge(Key::A)<2 || edge(Key::Tab)!=2 || edge(Key::Return)!=1 || edge(Key::Right)!=1
        || !held[static_cast<std::size_t>(Key::S)] || !held[static_cast<std::size_t>(Key::R)]
        || !sourceFrames || !nonzeroSource || !nonzeroFedToBackend || !audioConsumedInput || !audioPumpedSource || !tornDown || acceptedReloads!=1 || !throttleChanged)
        throw DesktopError("smoke validation","required simulation, native input, presentation, PCM or teardown evidence is missing");
    if (!options.virtualAudio && (audioDriver=="dummy" || audioDriver=="virtual-clocked-sink"))
        throw DesktopError("smoke validation","device mode cannot use a virtual endpoint");
}
void RunEvidence::writeReport() const {
    protectOutput(options.report,resourceRoot);
    const auto temporary=options.report.parent_path()/(options.report.filename().u8string()+".tmp-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    protectOutput(temporary,resourceRoot);
    std::ofstream out(temporary,std::ios::binary);
    out << std::setprecision(17) << "{\n  \"status\": " << jsonString(success?"pass":"fail") << ",\n  \"error\": " << jsonString(error)
        << ",\n  \"scope\": \"Native application execution; input is scheduled. Physical playback and acoustic latency require separate evidence.\""
        << ",\n  \"audio_mode\": " << jsonString(options.virtualAudio?"virtual":"device")
        << ",\n  \"video_driver\": " << jsonString(videoDriver) << ",\n  \"renderer\": " << jsonString(renderer)
        << ",\n  \"graphics_qualification\": \"Driver recorded; physical versus software GPU must be established by the host inventory.\""
        << ",\n  \"audio_driver\": " << jsonString(audioDriver) << ",\n  \"audio_device\": " << jsonString(audioDevice)
        << ",\n  \"device_frequency\": " << deviceFrequency << ",\n  \"device_channels\": " << deviceChannels
        << ",\n  \"device_format\": " << deviceFormat << ",\n  \"device_quantum\": " << deviceQuantum
        << ",\n  \"source_format\": \"44100 Hz, mono signed 16-bit PCM\""
        << ",\n  \"script\": " << jsonString(resolvedScript.u8string()) << ",\n  \"resources\": " << jsonString(resourceRoot.u8string())
        << ",\n  \"script_fnv1a64\": " << jsonString(std::to_string(scriptDigest)) << ",\n  \"source_pcm_fnv1a64\": " << jsonString(std::to_string(sourceDigest))
        << ",\n  \"presented_frames\": " << frames << ",\n  \"process_calls\": " << processCalls << ",\n  \"simulation_steps\": " << simulationSteps
        << ",\n  \"cylinders\": " << cylinders << ",\n  \"peak_rpm\": " << peakRpm << ",\n  \"model_packets\": " << modelPackets
        << ",\n  \"generated_packets\": " << generatedPackets << ",\n  \"text_packets\": " << textPackets
        << ",\n  \"readbacks\": " << screenshots << ",\n  \"nonbackground_pixels\": " << coloredPixels
        << ",\n  \"submitted_source_frames\": " << sourceFrames << ",\n  \"nonzero_source_frames\": " << nonzeroSource
        << ",\n  \"pumped_source_frames\": " << audioPumpedSource << ",\n  \"SDL_or_virtual_input_consumed\": " << audioConsumedInput
        << ",\n  \"backend_progress_semantics\": " << jsonString(audioDriver=="DirectSound"?"Safe write cursor advance; not a physical playback clock":"Consumed input frames; not a physical playback clock")
        << ",\n  \"nonzero_frames_fed_to_backend\": " << nonzeroFedToBackend
        << ",\n  \"silence_frames\": " << silenceFrames << ",\n  \"maximum_queued_frames\": " << queueMaximum
        << ",\n  \"resized\": " << (resized?"true":"false") << ",\n  \"teardown_complete\": " << (tornDown?"true":"false")
        << ",\n  \"accepted_reloads\": " << acceptedReloads << ",\n  \"throttle_changed\": " << (throttleChanged?"true":"false")
        << ",\n  \"accepted_key_edges\": [";
    for (std::size_t i=0;i<edges.size();++i) { if (i) out << ','; out << edges[i]; }
    out << "],\n  \"observed_key_holds\": [";
    for (std::size_t i=0;i<held.size();++i) { if (i) out << ','; out << held[i]; }
    out << "]\n}\n";
    out.close();
    if (!out) { std::error_code error; fs::remove(temporary,error); throw DesktopError("report","failed to write " + options.report.u8string()); }
    fs::rename(temporary,options.report);
}
int runApplication(const std::vector<std::string> &arguments,
                   const std::function<std::unique_ptr<Host>(const RunOptions &)> &factory) {
    std::shared_ptr<RunEvidence> evidence;
    try {
        const auto options=parseRunOptions(arguments);
        if (options.help) { std::cout << "Engine Simulator [--resources ROOT] [--script FILE] [--audio-mode device|virtual]\n"
            "  --smoke --frames 240 --report ABSOLUTE_NEW_JSON [--script ABSOLUTE_MR]\n"; return 0; }
        if (options.smoke) { evidence=std::make_shared<RunEvidence>(); evidence->options=options; }
        ObservedApplication app(evidence);
        if (evidence) {
            const auto paths=engine_sim::ResourcePaths::resolve(engine_sim::executablePath(),options.resources,true);
            evidence->resolvedScript=paths.script; evidence->resourceRoot=paths.root;
            protectOutput(options.report,paths.root); evidence->scriptDigest=fileDigest(paths.script);
            evidence->onFrame=[&app](unsigned frame) { app.pauseForSmoke(frame); };
        }
        auto host=factory(options); host->setRunEvidence(evidence);
        app.initialize(std::move(host),options.resources); app.run(); app.destroy();
        if (evidence) { evidence->validate(); evidence->success=true; evidence->writeReport(); }
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        if (evidence) {
            evidence->error=error.what();
            try { evidence->writeReport(); } catch (const std::exception &reportError) { std::cerr << reportError.what() << '\n'; }
        }
        return 1;
    }
}
}
