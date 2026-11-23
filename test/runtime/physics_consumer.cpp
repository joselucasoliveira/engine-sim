#include "definition.hpp"
#include "physics_scenario.hpp"

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

// The MIT Runtime Single fixture written as owned model values. Keep the
// curve and journal order evaluated by its original fixture entry.
cfg::Document fixture() {
    cfg::Document model;
    auto &part = model.components;
    part.functions = {
        {{"port"}, {0.002}, {{{0}, {0}}, {{0.004}, {0.0013743341139687937}}, {{0.008}, {0.0024738014051438285}}}},
        {{"lobe"}, {0.3490658503988889}, {
            {{-1.7453292519944443}, {0}}, {{-1.0471975511966667}, {0.002}},
            {{0}, {0.008}}, {{1.0471975511966667}, {0.002}}, {{1.7453292519944443}, {0}}
        }},
        {{"timing"}, {104.71975499999999}, {{{0}, {0.26179938779916667}}, {{628.31853}, {0.3490658503988889}}}},
        {{"flame"}, {5}, {}},
        {{"turbulence"}, {1}, {}},
    };
    part.functions[3].samples.push_back({{0}, {3}});
    for (int i = 1; i < 10; ++i) part.functions[3].samples.push_back({{5.0 * i}, {7.5 * i}});
    for (int i = 0; i < 30; ++i) part.functions[4].samples.push_back({{double(i)}, {0.5 * i}});
    part.journals = {{{"journal"}, {0}}};
    part.crankshafts = {
        {{"crank"}, {0.04}, {4}, {4}, {1}, {0.12}, {0}, {0}, {1.570796326795}, {{"journal"}}},
    };
    part.pistons = {{{"piston"}, {0.4}, {0}, {0.03}, {0}, {0}}};
    part.rods = {{{"rod"}, {0.3}, {0.001}, {0}, {0.15}, {0}, {}}};
    part.fuels = {{{"fuel"}, "Gasoline [Default]", {0.1}, {48100000}, {754.9999999999998},
                   {12.5}, {"flame"}, {0.8}, {0}, {0.6}, {2}, {10}}};
    part.throttles = {cfg::DirectThrottle{{"throttle"}, {1}}};
    const cfg::Cylinder cylinder{{"piston"}, {"rod"}, {"journal"}, {"intake"}, {"exhaust"}, {"wire"}, {0}, {1}};
    part.banks = {{{"bank"}, {0}, {0.08}, {0.22}, {0}, {0}, {0.5}, {cylinder}, {"head"}}};
    part.heads = {{{"head"}, {4e-5}, {0.00015}, {0.0008}, {0.00015}, {0.0008},
                   false, {"port"}, {"port"}, {"valves"}}};
    part.valvetrains = {cfg::Standard{{"valves"}, {"intake-cam"}, {"exhaust-cam"}}};
    part.camshafts = {
        {{"intake-cam"}, {0}, {0.012}, {"lobe"}, {{8.203047484373888}}},
        {{"exhaust-cam"}, {0}, {0.012}, {"lobe"}, {{4.363323129986111}}},
    };
    part.intakes = {{{"intake"}, {0.001}, {0.001}, {0.0025480505140388133}, {0},
                    {0.006370126285097034}, {12.5}, {0.95}, {0.10160000000000001}, {0.25}}};
    part.exhausts = {{{"exhaust"}, {0.6}, {0.001}, {0.006370126285097034}, {0.4},
                     {0.004777594713822776}, {1}, {1}, {"impulse"}}};
    part.impulses = {{{"impulse"}, "impulse.wav", {0.01}}};
    part.wires = {{{"wire"}}};
    part.ignitions = {{{"ignition"}, {"timing"}, {628.31853}, {0.5}, {{{"wire"}, {0}}}}};

    model.engine = {{"engine"}, "Runtime Single", {628.31853}, {62.831852999999995}, {40},
                    {104.71975499999999}, {628.31853}, {10.4719755},
                    {"fuel"}, {"throttle"}, {10000}, {0.01}, {0}, {0},
                    {{"crank"}}, {{"bank"}}, {"ignition"}, {"turbulence"}};
    model.selected_engine = model.engine.id;
    return model;
}

template<class Value>
Value checked(std::variant<Value, cfg::Diagnostic> result) {
    if (const auto *diagnostic = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(diagnostic->code + " " + diagnostic->pointer + " " + diagnostic->message);
    return std::get<Value>(std::move(result));
}

void run(std::ofstream *output) {
    const auto model = fixture();
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
