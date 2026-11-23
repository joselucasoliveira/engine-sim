#ifndef ENGINE_SIM_EXAMPLE_CANONICAL_HPP
#define ENGINE_SIM_EXAMPLE_CANONICAL_HPP

#include <engine_sim/config-json.hpp>

#include "model.hpp"

#include <ostream>
#include <stdexcept>
#include <utility>

namespace engine_sim::example {

inline std::string canonical_model(std::ostream &diagnostics) {
    namespace config = config_v1;
    auto document = runtime_single();
    auto &journal = document.components.banks.at(0).cylinders.at(0).rod_journal;
    auto valid_journal = journal;
    journal.value = "missing";
    auto invalid = config::normalize(document);
    const auto *problem = std::get_if<config::Diagnostic>(&invalid);
    if (!problem || problem->code != "CFG_REF" ||
        problem->pointer != "/components/banks/0/cylinders/0/rod_journal") {
        throw std::runtime_error("Unexpected diagnostic for the missing rod journal");
    }
    diagnostics << problem->code << ' ' << problem->pointer << ' ' << problem->message << '\n';

    journal = std::move(valid_journal);
    auto result = config::normalize(std::move(document));
    if (const auto *error = std::get_if<config::Diagnostic>(&result)) {
        throw std::runtime_error(error->code + " " + error->pointer + " " + error->message);
    }
    const auto &model = std::get<config::NormalizedDocument>(result);
    if (model.topology().cylinders.size() != 1 || model.values().selected_engine.value != "engine") {
        throw std::runtime_error("Unexpected normalized engine topology");
    }

    auto bytes = config::write_json(model);
    auto parsed = config::read_json(bytes);
    const auto *loaded = std::get_if<config::NormalizedDocument>(&parsed);
    if (!loaded || config::write_json(*loaded) != bytes) {
        throw std::runtime_error("Canonical JSON did not round-trip byte for byte");
    }
    return bytes;
}

}

#endif
