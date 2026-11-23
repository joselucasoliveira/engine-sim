#ifndef ENGINE_SIM_ASSEMBLY_CAPACITY_HPP
#define ENGINE_SIM_ASSEMBLY_CAPACITY_HPP

#include <engine_sim/config-validation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace engine_sim::assembly {

inline constexpr std::size_t fleet_document_limit = 262144;
inline constexpr std::size_t fleet_definition_limit = 256;
inline constexpr std::size_t fleet_curve_limit = 1024;
inline constexpr std::size_t fleet_point_limit = 4096;
inline constexpr std::size_t fleet_wave_limit = 524288;
inline constexpr std::size_t fleet_cell_limit = 16 * 1024 * 1024;
inline constexpr std::size_t fleet_storage_limit = 8 * 1024 * 1024;
inline constexpr std::size_t fleet_cylinder_limit = 12;
inline constexpr std::size_t fleet_exhaust_limit = 2;

struct FleetShape {
    std::size_t cylinders = 0, cranks = 0, banks = 0, intakes = 0, exhausts = 0;
    std::size_t bodies = 0, constraints = 0, rows = 0, generators = 0;
    std::size_t reserved_rows = 0, reserved_generators = 0;
};

struct FleetCapacity {
    FleetShape predicted, assembled;
    std::uint32_t output_hz = 0, simulation_hz = 0;
    std::size_t definitions = 0, curve_points = 0;
    std::size_t document_bytes = 0, unique_wave_bytes = 0, decoded_sample_bytes = 0;
    std::array<std::size_t, fleet_exhaust_limit> taps{}, assembled_taps{};
    std::array<std::size_t, fleet_cylinder_limit> delay_lengths{}, assembled_delay_lengths{};
    std::size_t delay_total = 0, tap_total = 0;
    std::size_t recipe_bytes = 0, graph_bytes = 0, solver_bytes = 0;
    std::size_t delay_bytes = 0, dsp_bytes = 0, assembled_dsp_bytes = 0;
    std::size_t session_bytes = 0, retained_bytes = 0, modeled_overhead = 0;
};

struct FleetPreparation {
    std::uint32_t output_hz;
    FleetCapacity capacity{};
};

// The total changes only if the product and addition fit in size_t and the budget.
bool charge_capacity(std::size_t &total, std::size_t count, std::size_t width,
                     std::size_t ceiling = static_cast<std::size_t>(-1)) noexcept;
std::optional<std::size_t> fleet_cells(std::size_t logical_slots) noexcept;
std::optional<config_v1::Diagnostic> check_fleet_shape(FleetShape &shape, bool driven);
std::optional<config_v1::Diagnostic> check_fleet_model(const config_v1::NormalizedDocument &model,
                                                        FleetCapacity &capacity, bool driven);
std::optional<config_v1::Diagnostic> assign_delay(FleetCapacity &capacity, std::size_t cylinder,
                                                    double primary_m, double exhaust_m, std::string_view bank_id);
std::optional<config_v1::Diagnostic> assign_taps(FleetCapacity &capacity, std::size_t channel,
                                                   std::size_t taps, std::string_view exhaust_id);
config_v1::Diagnostic extent_error(std::string_view field, std::uintmax_t requested,
                                   std::uintmax_t limit, std::string_view component = {});

}
#endif
