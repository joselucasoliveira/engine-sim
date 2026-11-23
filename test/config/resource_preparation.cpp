#include "preparation.hpp"
#include "wave_reader.h"

#include <engine_sim/config-json.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace assembly = engine_sim::assembly;
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Phase = assembly::PreparationPhase;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

struct Workspace {
    fs::path previous = fs::current_path();
    fs::path directory;

    Workspace() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = fs::temp_directory_path() / ("engine-sim-preparation-" + std::to_string(now));
        require(fs::create_directory(directory), "Cannot create temporary workspace");
    }

    ~Workspace() {
        std::error_code error;
        fs::current_path(previous, error);
        if (!error) fs::remove_all(directory, error);
    }
};

Bytes read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg();
    require(size > 0 && size <= 16 * 1024 * 1024, "Cannot read fixture");
    Bytes bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    require(static_cast<bool>(stream.read(reinterpret_cast<char *>(bytes.data()), size)), "Incomplete fixture");
    return bytes;
}

void write(const fs::path &path, const Bytes &bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    require(!stream.fail(), "Cannot write generated fixture");
}

cfg::NormalizedDocument normalized(cfg::ModelResult result) {
    if (const auto *problem = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(problem->code + " " + problem->pointer + " " + problem->message);
    return std::get<cfg::NormalizedDocument>(std::move(result));
}

cfg::NormalizedDocument json(const fs::path &path) {
    const auto bytes = read(path);
    return normalized(cfg::read_json({reinterpret_cast<const char *>(bytes.data()), bytes.size()}));
}

Bytes wave(const std::vector<std::int16_t> &samples, std::uint32_t rate = 44100) {
    Bytes bytes;
    const auto text = [&](std::string_view value) { bytes.insert(bytes.end(), value.begin(), value.end()); };
    const auto word = [&](std::uint32_t value, int size) {
        for (int i = 0; i < size; ++i) {
            bytes.push_back(static_cast<unsigned char>(value & 0xff));
            value >>= 8;
        }
    };
    const auto data_size = static_cast<std::uint32_t>(samples.size() * 2);
    text("RIFF"); word(48 + data_size, 4); text("WAVEfmt "); word(16, 4);
    word(1, 2); word(1, 2); word(rate, 4); word(rate * 2, 4); word(2, 2); word(16, 2);
    text("JUNK"); word(3, 4); text("abc"); word(0, 1);
    text("data"); word(data_size, 4);
    for (const auto sample : samples) word(static_cast<std::uint16_t>(sample), 2);
    return bytes;
}

assembly::PreparedSession prepared(assembly::PreparationResult result) {
    if (const auto *problem = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(problem->code + " " + problem->pointer + " " + problem->message);
    return std::get<assembly::PreparedSession>(std::move(result));
}

void reject(assembly::PreparationResult result, std::string_view code, std::string_view pointer) {
    const auto *problem = std::get_if<cfg::Diagnostic>(&result);
    if (!problem) throw std::runtime_error("Invalid preparation returned a session");
    if (problem->code != code || problem->pointer != pointer)
        throw std::runtime_error(problem->code + " " + problem->pointer + " " + problem->message);
}

template<class Record>
Record copy_as(Record source, const char *id) {
    source.id.value = id;
    return source;
}

cfg::Document two_channels(cfg::Document value, std::string response) {
    auto &parts = value.components;
    auto bank = copy_as(parts.banks.front(), "second-bank");
    auto head = copy_as(parts.heads.front(), "second-head");
    auto piston = copy_as(parts.pistons.front(), "second-piston");
    auto rod = copy_as(parts.rods.front(), "second-rod");
    auto exhaust = copy_as(parts.exhausts.front(), "second-exhaust");
    auto impulse = copy_as(parts.impulses.front(), "second-impulse");
    impulse.path = std::move(response);
    impulse.volume = {0.375};
    exhaust.impulse_response = impulse.id;
    bank.head = head.id;
    auto &cylinder = bank.cylinders.front();
    cylinder.piston = piston.id;
    cylinder.connecting_rod = rod.id;
    cylinder.exhaust_system = exhaust.id;
    parts.banks.push_back(bank);
    parts.heads.push_back(head);
    parts.pistons.push_back(piston);
    parts.rods.push_back(rod);
    parts.exhausts.push_back(exhaust);
    parts.impulses.push_back(impulse);
    // Early authored definitions do not determine actual channel order.
    value.engine.banks.insert(value.engine.banks.begin(), bank.id);
    return value;
}

void resource_cases(const fs::path &fixture) {
    Workspace temp;
    const auto original = temp.directory / "bundle";
    const auto bundle = temp.directory / fs::u8path(u8"\u00e1udio-\u30a8\u30f3\u30b8\u30f3");
    fs::create_directory(original);
    fs::copy_file(fixture, original / "model.json");
    std::vector<std::int16_t> signal(10025, 17);
    signal.front() = -32768;
    signal[10024] = 1001;
    const auto good = wave(signal);
    write(original / "impulse.wav", good);
    fs::rename(original, bundle);
    fs::current_path(temp.directory);
    const auto authored = json(bundle / "model.json");
    const auto path_field = "/components/impulses/0/path";
    auto good_session = prepared(assembly::prepare_session(authored, bundle, {}));
    require(good_session.inputs.files.size() == 1 && good_session.inputs.files[0].bytes == good &&
            good_session.inputs.files[0].path == fs::canonical(bundle/"impulse.wav") &&
            good_session.inputs.root == fs::canonical(bundle), "Relocated Unicode file input was not pinned exactly");
    require(assembly::prepare_session(authored, bundle, {}, {}, &good_session.inputs).index() == 0,
            "Identical recipe inputs failed re-preparation");

    const auto dual = normalized(cfg::normalize(two_channels(authored.values(), "impulse.wav")));
    int reads = 0;
    int decodes = 0;
    auto one_read = prepared(assembly::prepare_session(dual, bundle, {}, {}, nullptr, [&](Phase phase, std::size_t) {
        if (phase == Phase::pin) {
            ++reads;
            write(bundle / "impulse.wav", wave({1}));
        }
        if (phase == Phase::decode) ++decodes;
    }));
    auto &synth = one_read.session->synthesis;
    require(reads == 1 && decodes == 1 && one_read.inputs.files.size() == 1 &&
            one_read.inputs.files.front().bytes == good && one_read.inputs.bindings.size() == 2,
            "Shared impulse bytes were reopened, duplicated or not retained");
    const auto check_channel = [&](int channel, float gain) {
        auto &filter = synth.m_filters[channel].convolution;
        require(filter.getSampleCount() == 10000 && filter.getImpulseResponse()[0] == gain * -32768 / 32767 &&
                filter.getImpulseResponse()[9999] == gain * 17 / 32767, "DSP gain, clip or channel order differs");
    };
    check_channel(0, 0.375f);
    check_channel(1, 0.01f);
    auto trimmed = prepared(assembly::prepare_session(authored, bundle, {}));
    require(trimmed.session->synthesis.m_filters[0].convolution.getSampleCount() == 0,
            "A real response below the threshold was not trimmed to zero taps");

    write(bundle/"impulse.wav", wave({101, -100, 100, 3}));
    auto single_tap = prepared(assembly::prepare_session(authored, bundle, {}));
    require(single_tap.session->synthesis.m_filters[0].convolution.getSampleCount() == 1,
            "The legacy threshold requires strictly greater than 100");

    reject(assembly::prepare_session(authored, fs::path("relative"), {}), "CFG_RESOURCE_PATH", "");
    auto rejected = [&](std::string locator) {
        auto values = authored.values();
        values.components.impulses[0].path = std::move(locator);
        const auto model = normalized(cfg::normalize(std::move(values)));
        reject(assembly::prepare_session(model, bundle, {}), "CFG_RESOURCE_PATH", path_field);
    };
    for (const char *name : {"../impulse.wav", "./impulse.wav", "/impulse.wav", "d//impulse.wav", "impulse.wav/",
                             "C:impulse.wav", "d\\impulse.wav", "CON.wav", "folder./impulse.wav",
                             "impulse.wav ", "d\t/impulse.wav", "missing.wav"})
        rejected(name);
    fs::create_directory(bundle/"directory");
    rejected("directory");

    const auto outside = temp.directory/"outside.wav";
    write(outside, good);
    std::error_code link_error;
    fs::create_symlink(outside, bundle/"escape.wav", link_error);
#ifdef _WIN32
    if (link_error) std::cout << "Native symlink creation unavailable: " << link_error.message() << '\n';
#else
    require(!link_error, "Cannot create native symlink fixture");
#endif
    if (!link_error) {
        rejected("escape.wav");
        fs::create_symlink("impulse.wav", bundle/"alias.wav");
        auto alias_model = normalized(cfg::normalize(two_channels(authored.values(), "alias.wav")));
        auto aliased = prepared(assembly::prepare_session(alias_model, bundle, {}));
        require(aliased.inputs.files.size() == 1 && aliased.inputs.bindings[1].locator == "alias.wav",
                "An in-root symlink alias lost its locator or duplicated the byte input");
        fs::copy_file(bundle/"impulse.wav", bundle/"copy.wav");
        fs::remove(bundle/"alias.wav");
        fs::create_symlink("copy.wav", bundle/"alias.wav");
        reject(assembly::prepare_session(alias_model, bundle, {}, {}, &aliased.inputs),
               "CFG_RESOURCE_BYTES", "/components/impulses/1/path");
    }

    auto truncated = good;
    truncated.pop_back();
    for (const auto &bytes : {truncated, wave({1,2},48000), wave({}), Bytes{'R','I','F'}}) {
        write(bundle/"impulse.wav", bytes);
        reject(assembly::prepare_session(authored, bundle, {}), "CFG_RESOURCE_FORMAT", path_field);
        reject(assembly::prepare_session(authored, bundle, {}, {}, &good_session.inputs),
               "CFG_RESOURCE_BYTES", path_field);
    }
    {
        std::ofstream huge(bundle/"impulse.wav", std::ios::binary | std::ios::trunc);
        huge.seekp(static_cast<std::streamoff>(engine_sim::maxImpulseWaveBytes));
        huge.put('\0');
        require(huge.good(), "Could not create a sparse oversized file");
    }
    reject(assembly::prepare_session(authored, bundle, {}), "CFG_RESOURCE_BYTES", path_field);
    auto values = authored.values();
    values.components.impulses[0].volume = {std::numeric_limits<double>::max()};
    const auto too_much_gain = cfg::normalize(std::move(values));
    const auto &gain_problem = std::get<cfg::Diagnostic>(too_much_gain);
    require(gain_problem.code == "CFG_NUMBER" && gain_problem.pointer == "/components/impulses/0/volume",
            "Float gain overflow reached resource assembly");
}

void lifetime_cases(const fs::path &fixture) {
    const auto base = json(fixture);
    Workspace temp;
    const auto root = temp.directory;
    write(root/"impulse.wav", wave({1024, -656, 1}));
    write(root/"second.wav", wave({777, -1234, 225, 100, 20, -11}));
    const auto dual = normalized(cfg::normalize(two_channels(base.values(), "second.wav")));
    const assembly::PreparationProfile free{assembly::Mode::free, 14321, 23.75, 403};
    auto active = prepared(assembly::prepare_session(dual, root, free));
    auto *prior = active.session.get();
    auto &simulator = *prior->simulator;
    require(simulator.getSimulationFrequency() == 14321 && prior->synthesis.getInputSampleRate() == 14321 &&
            simulator.m_starterMotor.m_maxTorque == 23.75 &&
            simulator.m_starterMotor.m_rotationSpeed == -dual.values().engine.starter_speed.value,
            "Public frequency or starter override reached the wrong stage");
    const auto saved_curve_value = prior->synthesis.m_filters[0].convolution.getImpulseResponse()[0];

    using Stop = std::pair<Phase, std::size_t>;
    const std::vector<Stop> expected{{Phase::definition, 0}, {Phase::pin, 0}, {Phase::pin, 1},
        {Phase::decode, 0}, {Phase::decode, 1}, {Phase::channel, 0}, {Phase::channel, 1}, {Phase::session, 0}};
    std::vector<Stop> observed;
    (void)prepared(assembly::prepare_session(dual, root, free, {}, nullptr,
        [&](Phase phase, std::size_t position) { observed.emplace_back(phase, position); }));
    require(observed == expected, "Candidate phases did not precede ownership transfer in order");
    for (const auto &stop : expected) {
        bool escaped = false;
        try {
            (void)assembly::prepare_session(dual, root, free, {}, nullptr, [&](Phase phase, std::size_t position) {
                if (Stop{phase, position} == stop) throw std::bad_alloc();
            });
        } catch (const std::bad_alloc &) {
            escaped = true;
        }
        require(escaped && active.session.get() == prior && prior->definition.engine->getRandomSeed() == 403 &&
                prior->synthesis.m_filters[0].convolution.getImpulseResponse()[0] == saved_curve_value,
                "Failed candidate damaged the previously prepared session");
    }
    int decodes = 0;
    write(root/"impulse.wav", wave({-1024, -656, 1}));
    reject(assembly::prepare_session(dual, root, free, {}, &active.inputs, [&](Phase phase, std::size_t) {
        if (phase == Phase::decode) ++decodes;
    }), "CFG_RESOURCE_BYTES", "/components/impulses/0/path");
    require(decodes == 0 && active.session.get() == prior, "Reset interpreted changed bytes");
    auto recovered = prepared(assembly::prepare_session(dual, root, free));
    require(recovered.session.get() != prior && recovered.inputs.files.size() == 2, "Failed to prepare fresh resources");

    auto fractional = base.values();
    fractional.engine.simulation_frequency = {6789.75};
    fractional.engine.starter_speed = {-37.625};
    const auto model = normalized(cfg::normalize(std::move(fractional)));
    reject(assembly::prepare_session(model, root, {}, "fractional"), "CFG_INITIALIZE", "/engine/simulation_frequency");
    reject(assembly::prepare_session(model, root, {assembly::Mode::driven, 12000, 1, 403}), "CFG_INITIALIZE", "");
    const auto desktop = prepared(assembly::prepare_session(model, root, {assembly::Mode::desktop, 0, 0, 403}));
    const auto &desk = *desktop.session->simulator;
    require(desk.getSimulationFrequency() == 6789 && desktop.session->synthesis.getInputSampleRate() == 10000 &&
            desk.m_starterMotor.m_rotationSpeed == 37.625, "Private desktop startup ordering or starter sign differs");
    auto invalid = base.values();
    invalid.engine.hf_gain = {std::numeric_limits<double>::max()};
    const auto too_much_hf = cfg::normalize(std::move(invalid));
    const auto &hf_problem = std::get<cfg::Diagnostic>(too_much_hf);
    require(hf_problem.code == "CFG_NUMBER" && hf_problem.pointer == "/engine/hf_gain",
            "Audio parameter overflow reached solver preparation");
}
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto fixture = fs::absolute(fs::u8path(argv[2]));
        const std::string_view mode = argv[1];
        if (mode == "resources") resource_cases(fixture);
        else if (mode == "lifetime") lifetime_cases(fixture);
        else return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
