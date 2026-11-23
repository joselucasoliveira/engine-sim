#include <engine_sim/config-v1.hpp>

#if defined(ATG_ENGINE_SIM_ENGINE_H) || defined(ATG_ENGINE_SIM_SIMULATOR_H) || \
    defined(ATG_SIMPLE_2D_CONSTRAINT_SOLVER_SCS_H) || defined(PIRANHA_PIRANHA_H) || \
    defined(NLOHMANN_JSON_VERSION_MAJOR) || defined(SDL_MAJOR_VERSION) || defined(_WINDOWS_)
#error "Configuration values must build without interpreter, physics, JSON or host headers"
#endif

#include <type_traits>

namespace cfg = engine_sim::config_v1;

static_assert(cfg::Document::format_version == 1);
static_assert(std::is_aggregate_v<cfg::Document>);
static_assert(std::is_same_v<decltype(cfg::Piston::mass), cfg::Measure<cfg::MassUnit>>);
static_assert(!std::is_assignable_v<decltype(cfg::Piston::mass) &, cfg::Measure<cfg::LengthUnit>>);
static_assert(!std::is_assignable_v<decltype(cfg::Cylinder::piston) &, cfg::Ref<cfg::ConnectingRodTag>>);
static_assert(std::is_same_v<decltype(cfg::Document::vehicle), std::optional<cfg::Vehicle>>);
static_assert(std::is_same_v<decltype(cfg::ResolvedDefaults::vehicle), cfg::Vehicle>);
static_assert(std::is_same_v<decltype(cfg::Desktop::color_background), std::int32_t>);
static_assert(std::is_same_v<cfg::Throttle, std::variant<cfg::DirectThrottle, cfg::Governor>>);
static_assert(std::is_same_v<cfg::Valvetrain, std::variant<cfg::Standard, cfg::Vtec>>);

int main() {
    const cfg::Document value{};
    return value.vehicle.has_value() || value.engine.simulation_frequency.value != 0.0;
}
