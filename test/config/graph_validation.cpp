#include <engine_sim/config-validation.hpp>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
static_assert(!std::is_default_constructible_v<cfg::NormalizedDocument>);
static_assert(!std::is_constructible_v<cfg::NormalizedDocument, cfg::Document>);

void expect(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

cfg::Document paired_fixture() {
    cfg::Document model;
    auto &engine = model.engine;
    engine.id = model.selected_engine = {"motor"};
    engine.name = "Synthetic two-bank topology";
    engine.redline = {650.0};
    engine.starter_speed = {-50.0};
    engine.simulation_frequency = {10'000.0};
    engine.fuel = {"fuel"};
    engine.throttle = {"linkage"};
    engine.crankshafts = {{"shaft"}};
    engine.banks = {{"rear"}, {"front"}};
    engine.ignition = {"ignition"};
    engine.mean_piston_speed_to_turbulence = {"curve"};
    auto &part = model.components;

    cfg::Function curve;
    curve.id = {"curve"};
    curve.filter_radius = {0.5};
    curve.samples = {{{0}, {2}}, {{0}, {3}}};
    part.functions = {curve};
    part.journals = {{{"root-pin"}, {0}}, {{"slave-pin"}, {0.5}}, {{"child-pin"}, {0.2}}};

    cfg::Crankshaft shaft;
    shaft.id = {"shaft"};
    shaft.crank_throw = {0.04};
    shaft.flywheel_mass = {2};
    shaft.mass = {4};
    shaft.moment_of_inertia = {0.12};
    shaft.journals = {{"root-pin"}};
    part.crankshafts = {shaft};
    part.pistons = {{{"piston"}, {0.4}, {}, {0.03}}};
    cfg::ConnectingRod master;
    master.id = {"master"};
    master.mass = {0.3};
    master.moment_of_inertia = {0.001};
    master.length = {0.15};
    master.slave_throw = {0.01};
    master.journals = {{"slave-pin"}};
    cfg::ConnectingRod child = master;
    child.id = {"child"};
    child.journals = {{"child-pin"}};
    part.rods = {master, child};

    cfg::Fuel fuel;
    fuel.id = {"fuel"};
    fuel.name = "Synthesis fixture";
    fuel.molecular_mass = {0.114};
    fuel.energy_density = {4.4e7};
    fuel.density = {740.0};
    fuel.molecular_afr = {12.5};
    fuel.turbulence_to_flame_speed_ratio = {"curve"};
    part.fuels = {fuel};
    part.throttles = {cfg::DirectThrottle{{"linkage"}, {1.0}}};

    cfg::Cylinder front;
    front.piston = {"piston"};
    front.connecting_rod = {"master"};
    front.rod_journal = {"root-pin"};
    front.intake = {"plenum"};
    front.exhaust_system = {"collector"};
    front.ignition_wire = {"wire"};
    cfg::Cylinder rear = front;
    rear.connecting_rod = {"child"};
    rear.rod_journal = {"slave-pin"};

    cfg::CylinderBank front_bank;
    front_bank.id = {"front"};
    front_bank.bore = {0.08};
    front_bank.deck_height = {0.22};
    front_bank.cylinders = {front};
    front_bank.head = {"front-head"};
    cfg::CylinderBank rear_bank = front_bank;
    rear_bank.id = {"rear"};
    rear_bank.angle = {0.4};
    rear_bank.cylinders = {rear};
    rear_bank.head = {"rear-head"};
    part.banks = {front_bank, rear_bank};

    cfg::Head head;
    head.id = {"front-head"};
    head.chamber_volume = {0.00004};
    head.intake_runner_volume = {0.0001};
    head.exhaust_runner_volume = {0.00012};
    head.intake_runner_cross_section_area = {0.0008};
    head.exhaust_runner_cross_section_area = {0.0009};
    head.intake_port_flow = {"curve"};
    head.exhaust_port_flow = {"curve"};
    head.valvetrain = {"train"};
    part.heads = {head, head};
    part.heads[1].id = {"rear-head"};
    part.valvetrains = {cfg::Standard{{"train"}, {"cam"}, {"cam"}}};
    part.camshafts = {{{"cam"}, {0.1}, {0.01}, {"curve"}, {{0.2}}}};

    cfg::Intake intake;
    intake.id = {"plenum"};
    intake.plenum_volume = {0.001};
    intake.plenum_cross_section_area = {0.001};
    intake.molecular_afr = {12.5};
    part.intakes = {intake};
    cfg::ExhaustSystem exhaust;
    exhaust.id = {"collector"};
    exhaust.length = {0.6};
    exhaust.collector_cross_section_area = {0.001};
    exhaust.impulse_response = {"impulse"};
    part.exhausts = {exhaust};
    part.impulses = {{{"impulse"}, "resource-must-not-exist/fixture.wav", {0.01}}};
    part.wires = {{{"wire"}}};
    part.ignitions = {{{"ignition"}, {"curve"}, {650}, {0.5}, {{{"wire"}, {0.2}}, {{"wire"}, {0.3}}}}};
    return model;
}

template<class Mutator>
void invalid(Mutator mutate, std::string_view code, std::string_view stage, std::string_view pointer,
             std::string_view field, std::string_view component = {}) {
    auto model = paired_fixture();
    mutate(model);
    const auto result = cfg::normalize(std::move(model));
    const auto *error = std::get_if<cfg::Diagnostic>(&result);
    if (!error) throw std::runtime_error("Invalid fixture produced a validated graph");
    if (error->code != code || error->stage != stage || error->pointer != pointer)
        throw std::runtime_error(error->code + " " + error->stage + " " + error->pointer + " " + error->message);
    expect(error->field && *error->field == field, "Diagnostic field did not identify its input");
    expect(component.empty() || (error->component_id && *error->component_id == component),
           "Diagnostic lost the owning component");
    expect(!error->path && !error->byte_offset && !error->line && !error->column,
           "Programmatic diagnostics must leave source spans absent");
}

void reference_rules() {
    invalid([](auto &m) { m.components.exhausts[0].id = {"plenum"}; },
            "CFG_DUPLICATE_ID", "graph", "/components/exhausts/0/id", "id", "plenum");
    invalid([](auto &m) { m.engine.fuel = {"missing"}; }, "CFG_REF", "graph", "/engine/fuel", "fuel", "motor");
    invalid([](auto &m) { m.components.banks[1].cylinders[0].intake = {"wire"}; },
            "CFG_REF", "graph", "/components/banks/1/cylinders/0/intake", "intake", "rear");
    invalid([](auto &m) { m.components.banks[1].cylinders[0].ignition_wire = {"Wire"}; },
            "CFG_REF", "graph", "/components/banks/1/cylinders/0/ignition_wire", "ignition_wire", "rear");
    invalid([](auto &m) { m.components.ignitions[0].posts[0].wire = {"collector"}; },
            "CFG_REF", "graph", "/components/ignitions/0/posts/0/wire", "wire", "ignition");
    invalid([](auto &m) { m.selected_engine = {"front"}; },
            "CFG_REF", "graph", "/selected_engine", "selected_engine");
    invalid([](auto &m) { m.components.wires.push_back({{"unused"}}); },
            "CFG_REF", "graph", "/components/wires/1/id", "id", "unused");
    invalid([](auto &m) { m.engine.name = std::string("a\0b", 3); },
            "CFG_UTF8", "normalize", "/engine/name", "name", "motor");
    invalid([](auto &m) { m.components.impulses[0].path = "\xed\xa0\x80"; },
            "CFG_UTF8", "normalize", "/components/impulses/0/path", "path", "impulse");
}

void topology_rules() {
    auto raw = paired_fixture();
    auto result = cfg::normalize(raw);
    if (const auto *e = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(e->pointer + " " + e->message);
    const auto model = std::get<cfg::NormalizedDocument>(std::move(result));
    const auto copy = model;
    const auto &graph = copy.topology();
    expect(graph.banks == std::vector<std::size_t>{1, 0} && graph.crankshafts == std::vector<std::size_t>{0},
           "Selected bank or crank order changed");
    expect(graph.cylinders.size() == 2 && graph.intakes == std::vector<std::size_t>{0} &&
           graph.exhausts == std::vector<std::size_t>{0}, "First-use routing duplicated shared definitions");
    const auto &child = graph.cylinders[0];
    const auto &master = graph.cylinders[1];
    expect(child.index.bank == 0 && child.index.local == 0 && child.index.global == 0 &&
           child.rod_definition == 1 && std::get<cfg::MasterRodJournalIndex>(child.index.journal).rod == 1 &&
           std::get<cfg::MasterRodJournalIndex>(child.index.journal).journal == 0 &&
           child.root_crank == 0, "Subordinate route lost its later master or actual index");
    expect(master.index.bank == 1 && master.index.local == 0 && master.index.global == 1 &&
           master.rod_definition == 0 && std::get<cfg::CrankJournalIndex>(master.index.journal).crankshaft == 0,
           "Master route lost its crank or global ordering");
    expect(child.intake == master.intake && child.exhaust == master.exhaust &&
           child.piston_definition == master.piston_definition &&
           graph.wire_cylinders.at(0) == std::vector<std::size_t>{0, 1} &&
           copy.values().components.ignitions[0].posts.size() == 2,
           "Valid sharing and repeated firing-wire assignments did not survive");
    raw.components.banks.clear();
    expect(copy.values().components.banks.size() == 2 && copy.values().vehicle.has_value(),
           "Validated value aliased authored storage or omitted a fallback");
    expect(copy.values().components.impulses[0].path == "resource-must-not-exist/fixture.wav",
           "Model validation altered source resource metadata");

    invalid([](auto &m) { m.components.banks[0].cylinders[0].connecting_rod = {"child"}; },
            "CFG_TOPOLOGY", "graph", "/components/banks/0/cylinders/0/connecting_rod", "connecting_rod", "front");
    invalid([](auto &m) { m.components.banks[0].head = {"rear-head"}; },
            "CFG_TOPOLOGY", "graph", "/components/banks/0/head", "head", "front");
    invalid([](auto &m) { m.components.rods[0].journals[0] = {"root-pin"}; },
            "CFG_TOPOLOGY", "graph", "/components/rods/0/journals/0", "journals", "master");
    invalid([](auto &m) { m.components.banks[0].cylinders[0].rod_journal = {"child-pin"}; },
            "CFG_TOPOLOGY", "graph", "/components/banks/1/cylinders/0/rod_journal", "rod_journal", "rear");
    invalid([](auto &m) { m.components.banks[0].cylinders.resize(1'024); },
            "CFG_LIMIT", "normalize", "/components/banks/0/cylinders", "cylinders", "front");
    invalid([](auto &m) { m.components.wires.resize(65'536); }, "CFG_LIMIT", "normalize", "/components/wires", "wires");
    invalid([](auto &m) { m.components.functions[0].filter_radius.value = 0.0; },
            "CFG_CURVE", "graph", "/components/functions/0/filter_radius", "filter_radius", "curve");
    invalid([](auto &m) { m.components.functions[0].samples[1].x.value = std::numeric_limits<double>::quiet_NaN(); },
            "CFG_NUMBER", "normalize", "/components/functions/0/samples/1/x", "x", "curve");
    invalid([](auto &m) { m.components.crankshafts[0].mass.value = std::numeric_limits<double>::max();
                          m.components.crankshafts[0].flywheel_mass.value = std::numeric_limits<double>::max(); },
            "CFG_NUMBER", "graph", "/components/crankshafts/0/mass", "mass", "shaft");
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Expected references or topology");
        const std::string_view test(argv[1]);
        if (test == "references") reference_rules();
        else if (test == "topology") topology_rules();
        else throw std::invalid_argument("Unknown graph test");
        return 0;
    }
    catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
