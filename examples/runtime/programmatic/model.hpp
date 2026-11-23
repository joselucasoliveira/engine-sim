#ifndef ENGINE_SIM_EXAMPLE_RUNTIME_SINGLE_HPP
#define ENGINE_SIM_EXAMPLE_RUNTIME_SINGLE_HPP

#include <engine_sim/config-v1.hpp>

namespace engine_sim::example {

// The MIT Runtime Single fixture written as owned model values. Keep the
// curve and journal order evaluated by its original fixture entry.
inline config_v1::Document runtime_single() {
    namespace cfg = config_v1;
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

}

#endif
