#include "snapshot.hpp"

#include <engine_sim/config-json.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace legacy = engine_sim::legacy;

void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

bool bits(double left, double right) {
    std::uint64_t first, second;
    static_assert(sizeof first == sizeof left);
    std::memcpy(&first, &left, sizeof first);
    std::memcpy(&second, &right, sizeof second);
    return first == second;
}

const cfg::Function &find_curve(const cfg::Components &parts, const cfg::Ref<cfg::FunctionTag> &id) {
    for (const auto &part : parts.functions)
        if (part.id == id) return part;
    throw std::runtime_error("Missing captured function");
}

std::string normalized(const legacy::Snapshot &snapshot) {
    auto result = cfg::normalize(snapshot.values);
    if (const auto *error = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(error->code + " " + error->pointer + " " + error->message);
    return cfg::write_json(std::get<cfg::NormalizedDocument>(result));
}

void values(const es_script::PreparationContext &source, const std::filesystem::path &missing) {
    auto accepted = legacy::capture(source);
    require(es_script::Compiler::observer() == nullptr, "Recorder leaked into the next preparation");
    const auto &doc = accepted.values;
    require(doc.engine.name == "Runtime Single" && doc.selected_engine == doc.engine.id &&
            bits(doc.engine.starter_torque.value, 40) &&
            bits(doc.engine.simulation_frequency.value, 10000), "Evaluated engine fields differ");
    const auto &parts = doc.components;
    require(bits(parts.banks.at(0).bore.value, 0.08) &&
            bits(parts.pistons.at(0).compression_height.value, 0.03) &&
            bits(parts.rods.at(0).length.value, 0.15), "Physical inputs differ");
    const auto &governor = std::get<cfg::Governor>(parts.throttles.at(0));
    require(bits(governor.min_v.value, -1.25) && bits(governor.max_v.value, 2.75) &&
            bits(governor.k_s.value, 1.5) && bits(governor.k_d.value, 81.25) && bits(governor.gamma.value, 0.625),
            "Evaluated governor inputs differ");
    const auto &valve = std::get<cfg::Vtec>(parts.valvetrains.at(0));
    require(bits(valve.manifold_vacuum.value, 94321.25) && bits(valve.min_throttle_position.value, 0.4375),
            "Evaluated VTEC inputs differ");
    require(parts.fuels.at(0).name == "Gasoline [Default]" &&
            bits(parts.fuels.at(0).burning_efficiency_randomness.value, 0) &&
            !find_curve(parts, parts.fuels.at(0).turbulence_to_flame_speed_ratio).samples.empty(),
            "Fuel response curve disappeared");
    const auto &mean = find_curve(parts, doc.engine.mean_piston_speed_to_turbulence);
    require(bits(mean.filter_radius.value, 1) && mean.samples.size() == 30 &&
            bits(mean.samples.at(29).x.value, 29) && bits(mean.samples.at(29).y.value, 14.5),
            "Generated mean-speed curve differs");
    require(accepted.impulses.size() == 1 && accepted.impulses.front().evaluated_filename == "impulse.wav" &&
            std::filesystem::equivalent(accepted.impulses.front().declaring_unit, source.entry) &&
            accepted.impulses.front().impulse == parts.impulses.at(0).id,
            "IR filename did not retain its declaring unit");
    require(accepted.ignored_inputs.size() == 1 && accepted.ignored_inputs.front().name == "throttle_gamma" &&
            bits(accepted.ignored_inputs.front().value, 1.75), "Ignored evaluated value differs");
    require(accepted.consumed_scripts.size() > 1 &&
            accepted.consumed_scripts.front().resolvedPath == std::filesystem::canonical(source.entry) &&
            accepted.consumed_scripts.front().bytes.find("harmonic_cam_lobe lift") != std::string::npos,
            "Selected evaluation did not retain owned input bytes");

    const auto before_failure = normalized(accepted);
    auto adverse = source;
    adverse.entry = missing;
    for (int i = 0; i < 2; ++i) {
        bool rejected = false;
        try {
            (void)legacy::capture(adverse);
        }
        catch (const es_script::Compiler::Error &failure) {
            require(failure.diagnostic.stage == "graph" &&
                    failure.diagnostic.message.find("Missing ignition module") != std::string::npos,
                    "Missing object did not yield its actual fatal diagnostic");
            rejected = true;
        }
        require(rejected && es_script::Compiler::observer() == nullptr,
                "Failure produced a partial snapshot or retained a recorder");
    }
    require(normalized(legacy::capture(source)) == before_failure, "Capture differed after a previous failure");

    std::vector<es_script::ScriptInput> unobserved;
    const auto ordinary = es_script::Compiler::prepare(source, unobserved);
    require(ordinary.engine && ordinary.engine->getCylinderCount() == 1 &&
            unobserved.size() == accepted.consumed_scripts.size() &&
            es_script::Compiler::observer() == nullptr, "Ordinary compiler path depends on the recorder");
}

void identity(const es_script::PreparationContext &source) {
    const auto first = legacy::capture(source);
    const auto second = legacy::capture(source);
    require(normalized(first) == normalized(second), "Independent evaluations changed values or traversal IDs");
    const auto &parts = first.values.components;
    const auto &basic = parts.camshafts.at(0);
    const auto &shared = parts.camshafts.at(1);
    const auto &high = parts.camshafts.at(2);
    require(basic.lobe_profile == shared.lobe_profile && basic.lobe_profile != high.lobe_profile,
            "Shared and equal but independent functions were conflated");
    require(basic.lobe_profile.value == "function-000002" && high.lobe_profile.value == "function-000003",
            "Function identifiers do not follow declared discovery order");
    const auto &curve = find_curve(parts, basic.lobe_profile);
    const auto &separate = find_curve(parts, high.lobe_profile);
    require(curve.samples.size() == 25 && curve.samples.size() == separate.samples.size() &&
            bits(curve.filter_radius.value, separate.filter_radius.value) &&
            bits(curve.samples.at(0).x.value, 0) && bits(curve.samples.at(0).y.value, 0.008) &&
            bits(curve.samples.at(1).x.value, curve.filter_radius.value) &&
            bits(curve.samples.at(2).x.value, -curve.filter_radius.value),
            "Harmonic side effects or order changed");
    for (std::size_t i = 0; i < curve.samples.size(); ++i)
        require(bits(curve.samples.at(i).x.value, separate.samples.at(i).x.value) &&
                bits(curve.samples.at(i).y.value, separate.samples.at(i).y.value),
                "Equal evaluated curves do not match exactly");
    const auto &head = parts.heads.at(0);
    require(head.intake_port_flow == head.exhaust_port_flow, "Shared port functions were split");
    const auto &port = find_curve(parts, head.intake_port_flow);
    require(port.samples.size() == 4 &&
            bits(port.samples.at(1).x.value, port.samples.at(2).x.value) &&
            port.samples.at(1).y.value < port.samples.at(2).y.value, "Equal-x insertion order was lost");
    const auto &cylinder = parts.banks.at(0).cylinders.at(0);
    require(cylinder.ignition_wire == parts.ignitions.at(0).posts.at(0).wire,
            "Cylinder append wire disagrees with the ignition post");
}
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    try {
        const std::string_view mode = argv[1];
        const es_script::PreparationContext source{argv[2], argv[3], argv[4]};
        if (mode == "values") values(source, argv[5]);
        else if (mode == "identity") identity(source);
        else if (mode == "dump") std::cout << normalized(legacy::capture(source));
        else return 2;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
