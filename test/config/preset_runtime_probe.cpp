#include "legacy_runtime.hpp"
#include "json_runtime.hpp"
#include "runtime.hpp"
#include "piston_engine_simulator.h"
#include "mechanical_observation.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>
#include <tuple>
#include <vector>

namespace engine_sim {
struct RuntimeTestAccess {
    static auto &session(es_runtime &runtime) { return *runtime.implementation.m_session; }
};
}

namespace {
namespace fs = std::filesystem;

struct ExhaustAccess : PistonEngineSimulator {
    static constexpr auto endpoints = &ExhaustAccess::m_exhaustFlowStagingBuffer;
};
struct IgnitionAccess : IgnitionModule {
    static constexpr auto plugs = &IgnitionAccess::m_plugs;
};

using Words = std::vector<std::uint64_t>;

std::uint64_t bits(double value) {
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite preset observation");
    std::uint64_t word;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

void check(es_status status, const es_error &error) {
    if (status != ES_OK) throw std::runtime_error(error.message);
}

struct Trace {
    Words routes, endpoints, firing, combustion, mechanics;
    bool heard = false;
    bool mechanism_nonzero = false;
    bool brake_nonzero = false;
    bool distinct_reactions = false;
};

using RuntimeOwner = std::unique_ptr<es_runtime, decltype(&es_destroy)>;

RuntimeOwner load(const fs::path &root, const fs::path &entry, bool canonical) {
    es_error error{};
    es_config config{};
    check(es_config_init(ES_ABI_V2, sizeof(config), &config, &error), error);
    config.seed = 403;
    es_runtime *handle = nullptr;
    check(es_create(&config, &handle, &error), error);
    RuntimeOwner owner(handle, es_destroy);
    if (canonical) {
        check(engine_sim::json_runtime::load_file(handle, entry, root, nullptr, &error), error);
    } else {
        check(engine_sim::legacy_runtime::load(handle, entry.u8string().c_str(), (root / "es").u8string().c_str(),
                      (root / "assets").u8string().c_str(), &error), error);
    }
    return owner;
}

auto desktop_values(const ApplicationSettings &settings) {
    return std::tie(settings.startFullscreen, settings.powerUnits, settings.torqueUnits,
                    settings.speedUnits, settings.pressureUnits, settings.boostUnits,
                    settings.colorBackground, settings.colorForeground, settings.colorShadow,
                    settings.colorHighlight1, settings.colorHighlight2, settings.colorPink,
                    settings.colorRed, settings.colorOrange, settings.colorYellow,
                    settings.colorBlue, settings.colorGreen);
}

const ApplicationSettings &selected_settings(es_runtime &runtime) {
    return engine_sim::RuntimeTestAccess::session(runtime).prepared.session->definition.applicationSettings;
}

Trace measure(const fs::path &root, const fs::path &entry, bool canonical) {
    es_error error{};
    Trace trace;
    auto owner = load(root, entry, canonical);
    auto *handle = owner.get();
    auto &session = engine_sim::RuntimeTestAccess::session(*handle);
    const auto &simulator = static_cast<const PistonEngineSimulator &>(*session.prepared.session->simulator);
    const auto &engine = *session.prepared.session->definition.engine;
    const auto ticks = std::size_t{24000} * simulator.getSimulationFrequency() / 44100;
    engine_sim_capture::MechanicalObservation mechanics(simulator, ticks);
    trace.mechanics.push_back(bits(engine.getDisplacement()));
    if (!(engine.getDisplacement() > 0)) throw std::runtime_error("Invalid prepared displacement");
    const auto *plugs = engine.getIgnitionModule()->*IgnitionAccess::plugs;
    trace.routes.push_back(static_cast<std::uint64_t>(engine.getExhaustSystemCount()));
    for (int i = 0; i < engine.getCylinderCount(); ++i) {
        const auto *piston = engine.getPiston(i);
        const auto bank = piston->getCylinderBank()->getIndex();
        const auto local = piston->getCylinderIndex();
        const auto *head = engine.getHead(bank);
        trace.routes.push_back(static_cast<std::uint64_t>(head->getExhaustSystem(local)->getIndex()));
        trace.routes.push_back(bits(head->getHeaderPrimaryLength(local)));
        trace.firing.push_back(plugs[i].enabled);
        trace.firing.push_back(bits(plugs[i].angle));
    }

    session.pull->setTickObserver([&](const Simulator &sim) {
        const auto &piston = static_cast<const PistonEngineSimulator &>(sim);
        const double applied_load = piston.getExternalLoadReactionNm();
        mechanics.record(applied_load, 0, 0);
        const auto &current = mechanics.tick(mechanics.count() - 1);
        trace.mechanics.push_back(bits(current.mechanismNm));
        trace.mechanics.push_back(bits(current.loadReactionNm));
        trace.mechanism_nonzero |= current.mechanismNm != 0;
        trace.brake_nonzero |= current.loadReactionNm != 0;
        trace.distinct_reactions |= current.loadReactionNm != 0 && current.mechanismNm != current.loadReactionNm;
        const auto *endpoints = piston.*ExhaustAccess::endpoints;
        for (int i = 0; i < engine.getExhaustSystemCount(); ++i)
            trace.endpoints.push_back(bits(endpoints[i]));
        trace.combustion.push_back(bits(engine.getOutputCrankshaft()->getCycleAngle()));
        for (int i = 0; i < engine.getCylinderCount(); ++i)
            trace.combustion.push_back(engine.getChamber(i)->isLit());
        for (int i = 0; i < engine.getCylinderCount(); ++i) {
            const auto &chamber = mechanics.cylinder(mechanics.count() - 1, static_cast<std::size_t>(i));
            trace.mechanics.push_back(bits(chamber.pressurePa));
        }
    });

    std::vector<float> pcm(16000);
    for (unsigned part = 0; part < 2; ++part) {
        const es_controls controls{sizeof(controls), ES_ABI_V2, 1, part == 0, part == 0 ? 0.2 : 0.5, part == 0 ? 0.0 : 1.0, 0};
        check(es_set_controls(handle, &controls, &error), error);
        const std::uint32_t frames = part == 0 ? 8000 : 16000;
        std::uint32_t written = 0;
        check(es_render(handle, frames, pcm.data(), frames, &written, &error), error);
        if (written != frames) throw std::runtime_error("Incomplete preset observation");
        for (std::uint32_t frame = 0; frame < frames; ++frame) trace.heard |= pcm[frame] != 0;
    }
    if (mechanics.count() != ticks + 1) throw std::runtime_error("Incomplete preset tick trace");
    return trace;
}

void save(const fs::path &file, const Words &words) {
    std::ofstream out(file, std::ios::binary);
    for (const auto word : words) {
        for (unsigned byte = 0; byte < 8; ++byte)
            out.put(static_cast<char>(word >> (8 * byte)));
    }
    if (!out) throw std::runtime_error("Cannot write preset witness");
}
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    try {
        const auto root = fs::u8path(argv[1]);
        if (std::string_view(argv[4]) == "--settings-only") {
            auto legacy = load(root, fs::u8path(argv[2]), false);
            auto canonical = load(root, fs::u8path(argv[3]), true);
            if (desktop_values(selected_settings(*legacy)) != desktop_values(selected_settings(*canonical)))
                throw std::runtime_error("Evaluated desktop settings differ");
            return 0;
        }
        const auto legacy = measure(root, fs::u8path(argv[2]), false);
        const auto canonical = measure(root, fs::u8path(argv[3]), true);
        if (legacy.routes != canonical.routes || legacy.endpoints != canonical.endpoints ||
            legacy.firing != canonical.firing || legacy.combustion != canonical.combustion ||
            legacy.mechanics != canonical.mechanics || !legacy.heard || !canonical.heard ||
            !canonical.mechanism_nonzero || !canonical.brake_nonzero || !canonical.distinct_reactions)
            throw std::runtime_error("Prepared routing, endpoint, ignition or shaft trace differs");
        const auto output = fs::u8path(argv[4]);
        fs::create_directory(output);
        save(output / "routes", canonical.routes);
        save(output / "endpoints", canonical.endpoints);
        save(output / "firing", canonical.firing);
        save(output / "combustion", canonical.combustion);
        save(output / "mechanics", canonical.mechanics);
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
