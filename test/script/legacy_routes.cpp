#include "resources.hpp"

#include <engine_sim/config-json.hpp>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;
namespace legacy = engine_sim::legacy;
namespace fs = std::filesystem;

void require(bool success, const char *message) {
    if (!success) throw std::runtime_error(message);
}

legacy::ResolvedSnapshot success(legacy::ResolveResult result) {
    if (const auto *diagnostic = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(diagnostic->code + " " + diagnostic->pointer + " " + diagnostic->message);
    return std::get<legacy::ResolvedSnapshot>(std::move(result));
}

void selection(const es_script::PreparationContext &source, const fs::path &root) {
    const auto captured = legacy::capture(source);
    const auto bound = success(legacy::resolve(captured, root));
    const auto &value = bound.model.values();
    require(value.engine.name == "Runtime Single" && value.selected_engine == value.engine.id &&
            value.engine.simulation_frequency.value == 10000, "Overwritten engine selection escaped capture");
    require(value.vehicle && value.vehicle->mass.value == 1500 && value.vehicle->drag_coefficient.value == 0.375 &&
            value.vehicle->diff_ratio.value == 3.25, "Wrong vehicle selection");
    require(value.transmission && value.transmission->max_clutch_torque.value == 320.5 &&
            value.transmission->gears.size() == 2 && value.transmission->gears.at(0).value == 2.75 &&
            value.transmission->gears.at(1).value == 0.875, "Transmission observed a mutation after selection");
    require(value.desktop && value.desktop->start_fullscreen && value.desktop->torque_units == "Nm" &&
            value.desktop->color_blue == 222, "Wrong effective host settings");
    require(value.initialization && value.initialization->chamber_pa == 101325 &&
            value.initialization->chamber_kelvin == 298.15, "Missing versioned initialization defaults");
    const auto &functions = value.components.functions;
    require(functions.at(1).samples.size() == 26 && functions.at(1).samples.back().x.value == 1.5 &&
            functions.at(1).samples.back().y.value == 0.01 && functions.at(2).samples.size() == 25,
            "A function mutation was lost, applied too early, or copied after selection");
    const auto repeat = success(legacy::resolve(legacy::capture(source), root));
    require(cfg::write_json(repeat.model) == cfg::write_json(bound.model),
            "Repeated VM executions produced different canonical selections");
}

void routing(const legacy::ResolvedSnapshot &selection, bool shared) {
    const auto &lines = selection.model.topology();
    const auto &parts = selection.model.values().components;
    require(lines.banks == std::vector<std::size_t>{0, 1} && lines.cylinders.size() == 2 &&
            parts.banks.at(0).bore.value == 0.083 && parts.banks.at(1).bore.value == 0.080,
            "Bank-major order depends on declaration rather than selection");
    require(lines.intakes == std::vector<std::size_t>{0} && lines.exhausts == std::vector<std::size_t>{0, 1} &&
            lines.cylinders.at(0).exhaust == 0 && lines.cylinders.at(1).exhaust == 1 &&
            parts.exhausts.at(0).id != parts.exhausts.at(1).id,
            "Equal-valued independent exhausts or a shared intake were merged incorrectly");
    const auto &master = std::get<cfg::MasterRodJournalIndex>(lines.cylinders.at(0).index.journal);
    require(master.rod == 1 && master.journal == 0 && lines.cylinders.at(0).root_crank == 0 &&
            std::holds_alternative<cfg::CrankJournalIndex>(lines.cylinders.at(1).index.journal),
            "A late master rod or its root crank was not bound");
    require(parts.banks.at(0).cylinders.at(0).primary_length.value == 0.3125 &&
            parts.banks.at(0).cylinders.at(0).sound_attenuation.value == 0.75 &&
            lines.wire_cylinders == std::vector<std::vector<std::size_t>>{{0}, {1}} &&
            parts.ignitions.at(0).posts.at(0).wire == parts.banks.at(0).cylinders.at(0).ignition_wire,
            "Ordered per-cylinder details or firing routes differ");
    require(parts.pistons.at(0).id != parts.pistons.at(1).id &&
            parts.pistons.at(0).mass.value == parts.pistons.at(1).mass.value,
            "Equal-valued independent piston definitions were conflated");

    require(selection.impulses.size() == (shared ? 1u : 2u) &&
            selection.waves.size() == selection.impulses.size(), "Wrong unique selected IR count");
    require((parts.exhausts.at(0).impulse_response == parts.exhausts.at(1).impulse_response) == shared,
            "Shared IR identity differs");
    if (shared) {
        require(selection.waves.at(0).relative_path == "left/response.wav" && selection.waves.at(0).samples == 4,
                "Unselected same-name right origin leaked into binding");
    }
    else {
        require(selection.waves.at(0).relative_path == "right/response.wav" &&
                selection.waves.at(1).relative_path == "left/response.wav" &&
                selection.waves.at(0).samples == 5 && selection.waves.at(1).samples == 4 &&
                selection.waves.at(0).bytes != selection.waves.at(1).bytes,
                "Distinct imported files with equal basenames were conflated");
    }
    for (const auto &item : selection.impulses) {
        const auto &wave = selection.waves.at(item.wave);
        require(item.origin.evaluated_filename == "response.wav" &&
                fs::equivalent(item.origin.declaring_unit.parent_path(), wave.canonical_path.parent_path()) &&
                item.origin.declaring_unit.parent_path() != selection.consumed_scripts.front().resolvedPath.parent_path() &&
                wave.rate == 44100 && wave.channels == 1 && wave.encoding_bits == 16,
                "IR was resolved against its entry or metadata changed");
    }
    require(selection.consumed_scripts.size() == 11, "Actual imported script input set differs");
}

void reject_model(legacy::ResolveResult result, std::string_view code, std::string_view pointer) {
    require(std::holds_alternative<cfg::Diagnostic>(result), "Unsupported input produced a model");
    const auto &failure = std::get<cfg::Diagnostic>(result);
    require(failure.code == code && failure.pointer == pointer, "Unexpected model/resource diagnostic");
    if (failure.stage == "resources")
        require(failure.component_id && failure.path && failure.message.find("declared by") != std::string::npos,
                "Resource failure lacks declaring unit or attempted path");
}
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    try {
        const std::string_view mode = argv[1];
        const es_script::PreparationContext source{argv[2], argv[3], argv[4]};
        const fs::path root = argv[5];
        if (mode == "unselected") {
            try { (void)legacy::capture(source); }
            catch (const es_script::Compiler::Error &error) {
                require(error.diagnostic.message.find("Script did not select an engine") != std::string::npos,
                        "No-selection failure lost the legacy diagnostic");
                return 0;
            }
            throw std::runtime_error("No-selection input produced a candidate");
        }
        if (mode == "selection") selection(source, root);
        else {
            auto bound = legacy::resolve(legacy::capture(source), root);
            if (mode == "routing" || mode == "shared") routing(success(std::move(bound)), mode == "shared");
            else if (mode == "unsupported") reject_model(std::move(bound), "CFG_LIMIT", "/engine/name");
            else if (mode == "outside") reject_model(std::move(bound), "CFG_RESOURCE_PATH", "/components/impulses/0/path");
            else if (mode == "format") reject_model(std::move(bound), "CFG_RESOURCE_FORMAT", "/components/impulses/0/path");
            else if (mode == "dump") std::cout << cfg::write_json(success(std::move(bound)).model);
            else if (mode == "inputs") {
                const auto value = success(std::move(bound));
                for (const auto &input : value.consumed_scripts)
                    std::cout << "script\t" << input.resolvedPath.u8string() << '\t' << input.bytes.size() << '\n';
                for (const auto &impulse : value.impulses) {
                    const auto &wave = value.waves.at(impulse.wave);
                    std::cout << "impulse\t" << impulse.origin.impulse.value << '\t' << wave.relative_path
                              << '\t' << impulse.origin.declaring_unit.u8string() << '\t'
                              << wave.canonical_path.u8string() << '\t' << wave.bytes.size() << '\t' << wave.samples << '\n';
                }
            }
            else return 2;
        }
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
