#include "definition.hpp"
#include "physics_scenario.hpp"
#include "../../examples/runtime/programmatic/model.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace assembly = engine_sim::assembly;
using namespace physics_test;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}


template<class Value>
Value checked(std::variant<Value, cfg::Diagnostic> result) {
    if (const auto *diagnostic = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(diagnostic->code + " " + diagnostic->pointer + " " + diagnostic->message);
    return std::get<Value>(std::move(result));
}

void run(std::ofstream *output) {
    const auto model = engine_sim::example::runtime_single();
    auto invalid = model;
    invalid.components.banks[0].cylinders[0].rod_journal.value = "missing";
    require(std::holds_alternative<cfg::Diagnostic>(cfg::normalize(std::move(invalid))),
            "The programmatic model admitted a missing rod journal");
    const auto normalized = checked(cfg::normalize(model));
    auto definition = checked(assembly::build_definition(normalized, Seed));
    auto &engine = *definition.engine;
    engine.calculateDisplacement();
    auto &transmission = *definition.transmission;
    std::unique_ptr<Simulator> owner(engine.createSimulator(definition.vehicle.get(), &transmission, Frequency));
    auto &simulator = static_cast<PistonEngineSimulator &>(*owner);
    if (transmission.getGear() != -1) transmission.changeGear(-1);
    transmission.setClutchPressure(0);
    simulator.m_dyno.m_enabled = simulator.m_dyno.m_hold = false;
    simulator.enableExternalLoad();

    const auto initial = observe(simulator, 0, false);
    require(simulator.getTimestep() == 1.0 / Frequency && simulator.acousticChannelCount() == 1,
            "Prepared physics rate or channel count differs");
    require(simulator.advanceTick(0).data == nullptr && observe(simulator, 0, false) == initial,
            "An undersized acoustic extent advanced the simulator");
    require(std::abs(initial[0] - 0.0004021238596594936) < 1e-15, "Unexpected initial displacement");
    if (output) save(*output, initial);

    bool reacted = false, turned = false, impulsed = false, moved = false;
    std::size_t next = 0;
    std::array<double, 2> channels{0.0, 7.0};
    for (std::size_t i = 0; i < Ticks; ++i) {
        if (next < Commands.size() && Commands[next].tick == i) {
            const auto command = Commands[next++];
            engine.setSpeedControl(command.demand);
            engine.getIgnitionModule()->m_enabled = command.ignition;
            simulator.m_starterMotor.m_enabled = command.starter;
            simulator.setExternalLoadNm(command.load);
        }
        const auto excitation = simulator.advanceTick(1);
        require(excitation.count == 1 && excitation.data != nullptr, "Physics returned the wrong excitation extent");
        channels[0] = excitation.data[0];
        require(channels[1] == 7.0, "A borrowed channel view wrote past its extent");
        const auto state = observe(simulator, channels[0], true);
        require(state[0] == initial[0] && state[7] > 0 && state[8] > 0 && state[9] > 0,
                "Invalid cylinder state after a standalone tick");
        for (const auto value : state) require(std::isfinite(value), "Invalid finite physics observation");
        reacted |= state[13] != 0 && state[16] != 0;
        turned |= state[4] != initial[4];
        impulsed |= channels[0] != 0;
        moved |= state[1] > 100;
        if (output) save(*output, state);
    }
    require(reacted && turned && impulsed && moved, "Fixture did not exercise shaft torque, pressure, RPM and excitation");
}
}

int main(int argc, char **argv) {
    try {
        require(argc == 1 || argc == 2, "Expected at most one observation output");
        std::ofstream output;
        if (argc == 2) {
            const auto path = std::filesystem::u8path(argv[1]);
            require(!std::filesystem::exists(path), "Physics observation already exists");
            output.open(path, std::ios::binary);
            require(bool(output), "Could not create physics observation");
        }
        run(argc == 2 ? &output : nullptr);
        if (argc == 2) {
            output.close();
            require(bool(output), "Could not finish physics observation");
        }
        std::cout << "Ran " << Ticks << " physics ticks with fixed geometry and no synthesis\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
