#include <engine_sim/config-v1.hpp>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template<class Value, class Tag>
const Value &findDefinition(const std::vector<Value> &values, const cfg::Ref<Tag> &id) {
    const auto entry = std::find_if(values.begin(), values.end(),
        [&](const Value &value) { return value.id == id; });
    if (entry == values.end()) throw std::runtime_error("Expected an owned fixture definition");
    return *entry;
}

cfg::Document makeFixture() {
    cfg::Document recipe;
    recipe.selected_engine = {"paired"};
    recipe.engine.id = {"paired"};
    recipe.engine.crankshafts = {{"crank"}};
    recipe.engine.banks = {{"rear"}, {"front"}};
    recipe.engine.throttle = {"governed"};

    auto &parts = recipe.components;
    parts.journals = {{{"master-pin"}, {0.25}}, {{"shaft-pin"}, {-0.5}}};
    cfg::Crankshaft shaft;
    shaft.id = {"crank"};
    shaft.journals = {{"shaft-pin"}};
    parts.crankshafts.push_back(shaft);

    cfg::ConnectingRod child;
    child.id = {"child"};
    cfg::ConnectingRod master;
    master.id = {"master"};
    master.journals = {{"master-pin"}};
    parts.rods = {child, master};
    parts.pistons = {{{"piston"}, {0.4}}};
    parts.intakes = {{{"shared-intake"}, {0.001}}};
    parts.functions = {{{"shared-curve"}, {0.5}, {{{1}, {4}}, {{1}, {9}}}}};
    parts.impulses = {{{"impulse"}, "licensed-impulse.wav", {0.05}}};

    cfg::ExhaustSystem left;
    left.id = {"header"};
    left.length = {0.7};
    left.impulse_response = {"impulse"};
    cfg::ExhaustSystem right = left;
    right.id = {"Header"};
    parts.exhausts = {left, right};

    cfg::Cylinder subordinate;
    subordinate.piston = {"piston"};
    subordinate.connecting_rod = {"child"};
    subordinate.rod_journal = {"master-pin"};
    subordinate.intake = {"shared-intake"};
    subordinate.exhaust_system = {"Header"};
    subordinate.ignition_wire = {"wire"};
    cfg::Cylinder independent = subordinate;
    independent.connecting_rod = {"master"};
    independent.rod_journal = {"shaft-pin"};
    independent.exhaust_system = {"header"};
    cfg::CylinderBank front;
    front.id = {"front"};
    front.head = {"front-head"};
    front.cylinders = {independent};
    cfg::CylinderBank rear;
    rear.id = {"rear"};
    rear.head = {"rear-head"};
    rear.cylinders = {subordinate};
    parts.banks = {front, rear};

    cfg::Head frontHead;
    frontHead.id = {"front-head"};
    frontHead.intake_port_flow = {"shared-curve"};
    frontHead.exhaust_port_flow = {"shared-curve"};
    frontHead.valvetrain = {"base-cams"};
    cfg::Head rearHead = frontHead;
    rearHead.id = {"rear-head"};
    rearHead.valvetrain = {"alternate-cams"};
    parts.heads = {frontHead, rearHead};
    parts.valvetrains.emplace_back(cfg::Standard{{"base-cams"}, {"cam"}, {"cam"}});
    cfg::Vtec alternate;
    alternate.id = {"alternate-cams"};
    alternate.intake_camshaft = alternate.exhaust_camshaft = {"cam"};
    alternate.vtec_intake_camshaft = alternate.vtec_exhaust_camshaft = {"high-cam"};
    parts.valvetrains.emplace_back(alternate);
    parts.camshafts = {{{"cam"}, {0.0}, {0.01}, {"shared-curve"}, {{0.25}}},
                       {{"high-cam"}, {-0.25}, {0.012}, {"shared-curve"}, {{0.5}}}};
    parts.throttles.emplace_back(cfg::DirectThrottle{{"linkage"}, {1}});
    cfg::Governor governor;
    governor.id = {"governed"};
    governor.min_speed = {60.0};
    parts.throttles.emplace_back(governor);

    parts.wires = {{{"wire"}}};
    recipe.engine.ignition = {"ignition"};
    parts.ignitions = {{{"ignition"}, {"shared-curve"}, {600.0}, {0.5}, {{{"wire"}, {0.3}}, {{"wire"}, {0.0}}}}};
    parts.fuels = {{{"fuel"}, "Fixture Fuel", {0.114}}};
    recipe.engine.fuel = {"fuel"};
    recipe.engine.mean_piston_speed_to_turbulence = {"shared-curve"};
    recipe.desktop.emplace();
    recipe.desktop->speed_units = "mph";
    return recipe;
}

cfg::Document independentCopy() {
    auto source = makeFixture();
    auto copy = source;
    source.components.banks[1].id.value = "changed";
    source.components.rods[1].journals.clear();
    source.components.functions[0].samples[1].y.value = -1;
    source.components.impulses[0].path.clear();
    source.desktop->speed_units = "changed";
    return copy;
}

void verify() {
    auto copy = independentCopy();
    auto recipe = std::move(copy);
    auto &parts = recipe.components;
    check(recipe.selected_engine == recipe.engine.id, "selected engine must remain owned");
    check(!recipe.vehicle.has_value() && !recipe.transmission.has_value(), "authored omission was lost");
    check(recipe.desktop && recipe.desktop->speed_units == "mph", "owned desktop values changed");
    check(parts.banks[0].id.value == "front" && parts.banks[1].id.value == "rear", "definition order changed");

    const auto &rear = findDefinition(parts.banks, recipe.engine.banks[0]).cylinders.at(0);
    const auto &front = findDefinition(parts.banks, recipe.engine.banks[1]).cylinders.at(0);
    check(&findDefinition(parts.intakes, rear.intake) == &findDefinition(parts.intakes, front.intake),
          "shared intake did not resolve to one owned definition");
    const auto &a = findDefinition(parts.exhausts, rear.exhaust_system);
    const auto &b = findDefinition(parts.exhausts, front.exhaust_system);
    check(&a != &b && a.length.value == b.length.value && a.impulse_response == b.impulse_response,
          "equal-valued exhaust definitions lost identity");
    check(findDefinition(parts.impulses, a.impulse_response).path == "licensed-impulse.wav",
          "owned resource string changed");

    const auto &rod = findDefinition(parts.rods, front.connecting_rod);
    check(rod.journals.at(0) == rear.rod_journal, "subordinate cylinder lost the master's journal");
    check(findDefinition(parts.journals, rod.journals[0]).angle.value == 0.25, "master journal angle changed");
    const auto &shaft = findDefinition(parts.crankshafts, recipe.engine.crankshafts[0]);
    check(shaft.journals.at(0) == front.rod_journal, "direct crank journal binding changed");
    const cfg::CylinderIndex rearIndex{0, 0, 0, cfg::MasterRodJournalIndex{1, 0}};
    const cfg::CylinderIndex frontIndex{1, 0, 1, cfg::CrankJournalIndex{0, 0}};
    check(rearIndex.global == 0 && frontIndex.local == 0 && frontIndex.global == 1 &&
          std::get<cfg::MasterRodJournalIndex>(rearIndex.journal).rod == 1 &&
          std::get<cfg::CrankJournalIndex>(frontIndex.journal).crankshaft == 0,
          "resolved index types did not preserve the two journal routes");

    const auto &curve = findDefinition(parts.functions, parts.heads[0].intake_port_flow);
    check(&curve == &findDefinition(parts.functions, parts.heads[1].exhaust_port_flow),
          "shared port curve changed");
    check(curve.samples.size()==2 && curve.samples[0].x.value==1 && curve.samples[1].x.value==1 &&
          curve.samples[0].y.value==4 && curve.samples[1].y.value==9, "equal-x sample order changed");
    check(std::get<cfg::DirectThrottle>(parts.throttles[0]).gamma.value == 1 &&
          std::get<cfg::Governor>(parts.throttles[1]).id == recipe.engine.throttle, "throttle alternative changed");
    check(std::get<cfg::Standard>(parts.valvetrains[0]).intake_camshaft.value == "cam" &&
          std::get<cfg::Vtec>(parts.valvetrains[1]).vtec_intake_camshaft.value == "high-cam",
          "valvetrain alternative changed");
    check(rear.ignition_wire == front.ignition_wire &&
          parts.ignitions[0].posts[1].wire == rear.ignition_wire &&
          parts.ignitions[0].posts[0].angle.value == 0.3 &&
          parts.ignitions[0].posts[1].angle.value == 0.0, "ordered wiring assignments changed");
}
}

int main() {
    try {
        verify();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
