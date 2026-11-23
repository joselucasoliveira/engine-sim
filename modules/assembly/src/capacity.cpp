#include "capacity.hpp"
#include "preparation.hpp"
#include "checked_solver.h"
#include "direct_throttle_linkage.h"
#include "governor.h"
#include "piston_engine_simulator.h"
#include "standard_valvetrain.h"
#include "vtec_valvetrain.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace engine_sim::assembly {
namespace cfg = config_v1;

config_v1::Diagnostic extent_error(std::string_view field, std::uintmax_t requested,
                                   std::uintmax_t limit, std::string_view component) {
    cfg::Diagnostic problem;
    problem.code = "CFG_LIMIT";
    problem.stage = "initialize";
    problem.field = field;
    if (!component.empty()) problem.component_id = component;
    problem.message = std::string(field) + " requested " + std::to_string(requested) +
        ", limit " + std::to_string(limit);
    return problem;
}

bool charge_capacity(std::size_t &total, std::size_t count, std::size_t width, std::size_t ceiling) noexcept {
    if (total > ceiling || (width != 0 && count > (ceiling - total) / width)) return false;
    total += count * width;
    return true;
}

std::optional<std::size_t> fleet_cells(std::size_t logical_slots) noexcept {
    if (logical_slots < 1 || logical_slots > 8) return {};
    return logical_slots + 3;
}

std::optional<cfg::Diagnostic> check_fleet_shape(FleetShape &shape, bool driven) {
    const auto dimension = [](std::string_view name, std::size_t requested, std::size_t limit)
            -> std::optional<cfg::Diagnostic> {
        if (requested == 0) {
            auto error = extent_error(name, requested, limit);
            error.message += ", positive value required";
            return error;
        }
        if (requested > limit) return extent_error(name, requested, limit);
        return {};
    };
    if (auto failure = dimension("cylinders", shape.cylinders, fleet_cylinder_limit)) return failure;
    if (auto failure = dimension("cranks", shape.cranks, 2)) return failure;
    if (auto failure = dimension("banks", shape.banks, 9)) return failure;
    if (auto failure = dimension("intakes", shape.intakes, 4)) return failure;
    if (auto failure = dimension("exhaust channels", shape.exhausts, fleet_exhaust_limit)) return failure;

    // The largest admitted inputs are checked above, so every intermediate is
    // representable, including the dependency's signed matrix dimensions.
    shape.bodies = shape.cranks + 2 * shape.cylinders + 1;
    shape.constraints = 3 * shape.cranks + 3 * shape.cylinders + 4;
    shape.rows = 4 * shape.cranks + 5 * shape.cylinders + 4;
    shape.generators = shape.cylinders + driven;
    shape.reserved_rows = 1;
    shape.reserved_generators = driven;
    if (auto failure = dimension("bodies", shape.bodies, 27)) return failure;
    if (auto failure = dimension("constraint objects", shape.constraints, 46)) return failure;
    if (auto failure = dimension("scalar rows", shape.rows, 72)) return failure;
    return dimension("force generators", shape.generators, 13);
}

std::optional<cfg::Diagnostic> assign_delay(FleetCapacity &capacity, std::size_t cylinder,
                                            double primary_m, double exhaust_m, std::string_view bank_id) {
    constexpr std::size_t slack = 32;
    constexpr auto per_cylinder = 1024U;
    constexpr auto sum_limit = 8192U;
    const double length = primary_m + exhaust_m;
    const double speed_of_sound = 343.0 * units::m / units::sec;
    const double latency = std::round((length / speed_of_sound) * 10000.0);
    if (cylinder >= capacity.delay_lengths.size())
        return extent_error("cylinder index", cylinder, capacity.delay_lengths.size() - 1, bank_id);
    if (!std::isfinite(latency) || length <= 0 || latency < 0 ||
        latency > static_cast<double>(std::numeric_limits<int>::max() - slack)) {
        auto issue = extent_error("delay ring samples", 0, per_cylinder, bank_id);
        issue.message = "Delay ring samples are not representable for cylinder " + std::to_string(cylinder);
        return issue;
    }
    const auto slots = static_cast<std::size_t>(latency) + slack;
    if (slots > per_cylinder) return extent_error("delay ring samples", slots, per_cylinder, bank_id);
    if (!charge_capacity(capacity.delay_total, slots, 1, sum_limit))
        return extent_error("total delay ring samples", capacity.delay_total + slots, sum_limit, bank_id);
    capacity.delay_lengths[cylinder] = slots;
    return {};
}

std::optional<cfg::Diagnostic> assign_taps(FleetCapacity &capacity, std::size_t channel,
                                           std::size_t taps, std::string_view exhaust_id) {
    if (channel >= capacity.taps.size())
        return extent_error("exhaust channel index", channel, capacity.taps.size() - 1, exhaust_id);
    if (taps > 10000) return extent_error("effective FIR taps", taps, 10000, exhaust_id);
    if (!charge_capacity(capacity.tap_total, taps, 1, 20000))
        return extent_error("total effective FIR taps", capacity.tap_total + taps, 20000, exhaust_id);
    capacity.taps[channel] = taps;
    return {};
}

std::optional<cfg::Diagnostic> check_fleet_model(const cfg::NormalizedDocument &model,
                                                FleetCapacity &capacity, bool driven) {
    const auto &tables = model.values().components;
    capacity.definitions = tables.functions.size() + tables.journals.size() + tables.crankshafts.size() +
        tables.pistons.size() + tables.rods.size() + tables.fuels.size() + tables.throttles.size() +
        tables.banks.size() + tables.heads.size() + tables.valvetrains.size() + tables.camshafts.size() +
        tables.intakes.size() + tables.exhausts.size() + tables.impulses.size() + tables.wires.size() + tables.ignitions.size();
    if (capacity.definitions > fleet_definition_limit)
        return extent_error("component definitions", capacity.definitions, fleet_definition_limit);
    for (const auto &curve : tables.functions) {
        const auto points = curve.samples.size();
        if (points > fleet_curve_limit) return extent_error("curve points", points, fleet_curve_limit, curve.id.value);
        if (!charge_capacity(capacity.curve_points, points, 1, fleet_point_limit))
            return extent_error("total curve points", capacity.curve_points + points, fleet_point_limit, curve.id.value);
    }
    const auto &routes = model.topology();
    capacity.predicted = {routes.cylinders.size(), routes.crankshafts.size(), routes.banks.size(),
                          routes.intakes.size(), routes.exhausts.size()};
    if (auto failure = check_fleet_shape(capacity.predicted, driven)) return failure;
    for (const auto &route : routes.cylinders) {
        const auto &bank = tables.banks[routes.banks[route.index.bank]];
        const auto &exhaust = tables.exhausts[routes.exhausts[route.exhaust]];
        if (auto failure = assign_delay(capacity, route.index.global, bank.cylinders[route.index.local].primary_length.value,
                                        exhaust.length.value, bank.id.value)) return failure;
    }
    return {};
}

std::size_t path_capacity(const std::filesystem::path &path) {
    // The native string plus a model for separately stored parsed components.
    // Filesystem implementation and allocator rounding are audited separately.
    using Path = std::filesystem::path;
    std::size_t result = (path.native().capacity() + 1) * sizeof(Path::value_type);
    for (const auto &component : path) {
        result += sizeof(Path) + (component.native().capacity() + 1) * sizeof(Path::value_type);
    }
    return result;
}

bool account_pinned(FleetCapacity &report, const ResourceInputs &inputs) {
    auto &bytes = report.recipe_bytes;
    if (!charge_capacity(bytes, inputs.files.capacity(), sizeof(ResourceInput)) ||
        !charge_capacity(bytes, inputs.bindings.capacity(), sizeof(ResourceBinding)) ||
        !charge_capacity(bytes, path_capacity(inputs.root), 1)) return false;
    for (const auto &file : inputs.files) {
        if (!charge_capacity(report.unique_wave_bytes, file.bytes.size(), 1, fleet_wave_limit) ||
            !charge_capacity(bytes, file.bytes.capacity(), 1) ||
            !charge_capacity(bytes, path_capacity(file.path), 1)) return false;
    }
    for (const auto &binding : inputs.bindings) {
        if (!charge_capacity(bytes, binding.locator.capacity() + 1, 1)) return false;
    }
    return true;
}

namespace {

template<class T> const auto &record_key(const T &value) { return value.id.value; }
template<class... T> const auto &record_key(const std::variant<T...> &value) {
    return std::visit([](const auto &item) -> const std::string & { return record_key(item); }, value);
}

template<class Records, class Tag>
const auto &lookup(const Records &records, const cfg::Ref<Tag> &ref) {
    // Normalization has checked the identity and type of every used reference.
    return *std::find_if(records.begin(), records.end(), [&](const auto &value) { return record_key(value) == ref.value; });
}

constexpr std::size_t buffer_frames = 44100;
constexpr std::size_t jitter_samples = 10;
constexpr std::size_t butterworth_history = 8;

std::size_t graph_bytes(const cfg::NormalizedDocument &model, const FleetShape &shape) {
    const auto &root = model.values();
    const auto &routes = model.topology();
    const auto &table = root.components;
    const auto &fuel = lookup(table.fuels, root.engine.fuel);
    const auto &ignition = lookup(table.ignitions, root.engine.ignition);
    std::size_t bytes = sizeof(EngineSession) + sizeof(Engine) + sizeof(Vehicle) + sizeof(Transmission) +
        sizeof(PistonEngineSimulator) + sizeof(atg_scs::OptimizedNsvRigidBodySystem) + sizeof(CheckedSolver);
    bytes += root.engine.name.capacity() + fuel.name.capacity() + 2;
    if (root.desktop) {
        const auto &desktop = *root.desktop;
        bytes += desktop.power_units.capacity() + desktop.torque_units.capacity() + desktop.speed_units.capacity() +
            desktop.pressure_units.capacity() + desktop.boost_units.capacity() + 5;
    }
    bytes += std::holds_alternative<cfg::DirectThrottle>(lookup(table.throttles, root.engine.throttle))
        ? sizeof(DirectThrottleLinkage) : sizeof(Governor);
    bytes += (root.transmission ? root.transmission->gears.size() : 6) * sizeof(double);
    bytes += shape.cranks * (sizeof(Crankshaft) + sizeof(atg_scs::FixedPositionConstraint) +
        sizeof(atg_scs::RotationFrictionConstraint));
    bytes += (shape.cranks - 1) * sizeof(atg_scs::ClutchConstraint);
    bytes += shape.banks * (sizeof(CylinderBank) + sizeof(CylinderHead));
    bytes += shape.cylinders * (sizeof(Piston) + sizeof(ConnectingRod) + sizeof(CombustionChamber) +
        sizeof(CylinderHead::Cylinder) + sizeof(IgnitionModule::SparkPlug) + sizeof(atg_scs::LineConstraint) +
        2 * sizeof(atg_scs::LinkConstraint) + 2 * 256 * sizeof(double));
    bytes += shape.intakes * sizeof(Intake) + shape.exhausts * (sizeof(ExhaustSystem) + sizeof(double));
    bytes += Simulator::DynoTorqueSamples * sizeof(double);
    for (auto crank : routes.crankshafts) bytes += table.crankshafts[crank].journals.size() * sizeof(double);
    for (const auto &route : routes.cylinders) {
        bytes += table.rods[route.rod_definition].journals.size() * sizeof(double);
        const auto &exhaust = table.exhausts[routes.exhausts[route.exhaust]];
        bytes += sizeof(ImpulseResponse) + lookup(table.impulses, exhaust.impulse_response).path.capacity() + 1;
    }

    std::array<bool, fleet_definition_limit> used_curves{};
    std::size_t resources = shape.cylinders; // The legacy builder owns one IR descriptor per cylinder.
    const auto curve = [&](const cfg::Ref<cfg::FunctionTag> &ref) {
        const auto &function = lookup(table.functions, ref);
        const auto index = static_cast<std::size_t>(&function - table.functions.data());
        if (used_curves[index]) return;
        used_curves[index] = true;
        bytes += sizeof(Function) + function.samples.size() * 2 * sizeof(double);
        ++resources;
    };
    const auto cam = [&](const cfg::Ref<cfg::CamshaftTag> &ref) {
        const auto &camshaft = lookup(table.camshafts, ref);
        bytes += sizeof(Camshaft) + camshaft.lobes.size() * sizeof(double);
        ++resources;
        curve(camshaft.lobe_profile);
    };
    curve(root.engine.mean_piston_speed_to_turbulence);
    curve(fuel.turbulence_to_flame_speed_ratio);
    curve(ignition.timing_curve);
    for (auto bank : routes.banks) {
        const auto &head = lookup(table.heads, table.banks[bank].head);
        curve(head.intake_port_flow);
        curve(head.exhaust_port_flow);
        const auto &train = lookup(table.valvetrains, head.valvetrain);
        std::visit([&](const auto &value) {
            cam(value.intake_camshaft);
            cam(value.exhaust_camshaft);
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, cfg::Vtec>) {
                bytes += sizeof(VtecValvetrain);
                cam(value.vtec_intake_camshaft);
                cam(value.vtec_exhaust_camshaft);
            } else {
                bytes += sizeof(StandardValvetrain);
            }
        }, train);
        ++resources;
    }
    // The engine and solver vectors grow by at most 2x on the supported STL
    // implementations. Allow control blocks separately in modeled_overhead.
    bytes += 2 * resources * sizeof(std::shared_ptr<void>);
    return bytes;
}

std::size_t solver_bytes(const FleetShape &shape) {
    const auto n = shape.bodies;
    const auto r = shape.rows;
    const auto dense = [](std::size_t width, std::size_t height) {
        return height * (width * sizeof(double) + sizeof(double *));
    };
    const auto sparse = r * (6 * sizeof(double) + 2 * sizeof(std::uint8_t) + sizeof(double *));
    const auto state = 13 * n * sizeof(double) + r * (sizeof(int) + 6 * sizeof(double));
    return state + dense(r, r) + 5 * dense(1, 3 * n) + 5 * dense(1, r) + dense(2, r) +
        2 * dense(1, std::max(3 * n, r)) + 3 * sparse +
        4 * atg_scs::RigidBodySystem::ProfilingSamples * sizeof(long long) +
        2 * (n + shape.constraints + shape.generators) * sizeof(void *);
}

}

bool account_session(FleetCapacity &report, const cfg::NormalizedDocument &model) {
    const auto &shape = report.predicted;
    // All arithmetic here uses the already admitted fixed cardinalities and
    // normalized schema extents. The category sum is still checked in size_t.
    report.graph_bytes += graph_bytes(model, shape);
    report.solver_bytes = solver_bytes(shape);
    report.delay_bytes = shape.cylinders * sizeof(DelayFilter) + report.delay_total * sizeof(double);
    report.dsp_bytes = 2 * buffer_frames * sizeof(std::int16_t) + butterworth_history * sizeof(float) +
        shape.exhausts * (sizeof(Synthesizer::InputChannel) + sizeof(Synthesizer::ProcessingFilters) +
            sizeof(std::vector<float>) + (3 * buffer_frames + jitter_samples + 2 * butterworth_history) * sizeof(float) +
            butterworth_history * sizeof(double)) + 2 * report.tap_total * sizeof(float);
    // Per-allocation allocator/control-block model, distinct from measured RSS.
    report.modeled_overhead = 64 * (report.definitions * 4 + shape.cylinders * 8 + shape.banks * 16 + 192);
    for (auto bytes : {report.recipe_bytes, report.graph_bytes, report.solver_bytes, report.delay_bytes,
                       report.dsp_bytes, report.session_bytes, report.modeled_overhead}) {
        if (!charge_capacity(report.retained_bytes, bytes, 1)) return false;
    }
    return report.retained_bytes <= fleet_cell_limit;
}

struct CapacityInspection {
    static bool compare(FleetCapacity &report, const PistonEngineSimulator &simulator, const Synthesizer &synth) {
        const auto &shape = report.predicted;
        if (synth.m_inputChannelCount != static_cast<int>(shape.exhausts) ||
            synth.m_pendingInput.size() != shape.exhausts || synth.m_audioBuffer.capacity() != buffer_frames ||
            synth.m_renderedAudio.size() != buffer_frames || synth.m_inputBufferSize != static_cast<int>(buffer_frames) ||
            synth.m_audioSampleRate != 44100.0f || synth.m_inputSampleRate != static_cast<float>(report.simulation_hz)) return false;
        report.assembled_dsp_bytes = (synth.m_audioBuffer.capacity() + synth.m_renderedAudio.capacity()) * sizeof(std::int16_t) +
            butterworth_history * sizeof(float) + synth.m_pendingInput.capacity() * sizeof(std::vector<float>);
        for (std::size_t i = 0; i < shape.exhausts; ++i) {
            const auto &input = synth.m_inputChannels[i];
            const auto &filter = synth.m_filters[i];
            report.assembled_taps[i] = static_cast<std::size_t>(filter.convolution.getSampleCount());
            if (input.data.capacity() != buffer_frames || synth.m_pendingInput[i].size() != buffer_frames ||
                report.assembled_taps[i] != report.taps[i]) return false;
            report.assembled_dsp_bytes += sizeof(input) + sizeof(filter) +
                (input.data.capacity() + buffer_frames + synth.m_pendingInput[i].capacity() + jitter_samples +
                    2 * butterworth_history) * sizeof(float) + butterworth_history * sizeof(double) +
                2 * report.assembled_taps[i] * sizeof(float);
        }
        for (std::size_t i = 0; i < shape.cylinders; ++i) {
            report.assembled_delay_lengths[i] = simulator.m_delayFilters[i].m_history.capacity();
            if (report.assembled_delay_lengths[i] != report.delay_lengths[i]) return false;
        }
        return report.assembled_dsp_bytes == report.dsp_bytes;
    }
};

bool verify_session(FleetCapacity &report, EngineSession &built) {
    auto *simulator = dynamic_cast<PistonEngineSimulator *>(built.simulator.get());
    const auto *engine = built.definition.engine.get();
    if (!simulator || !engine || simulator->getFluidSimulationSteps() != 8 ||
        simulator->getSimulationFrequency() != static_cast<int>(report.simulation_hz)) return false;
    const auto *system = dynamic_cast<atg_scs::OptimizedNsvRigidBodySystem *>(simulator->getSystem());
    if (!system) return false;
    auto &actual = report.assembled;
    actual = {static_cast<std::size_t>(engine->getCylinderCount()), static_cast<std::size_t>(engine->getCrankshaftCount()),
              static_cast<std::size_t>(engine->getCylinderBankCount()), static_cast<std::size_t>(engine->getIntakeCount()),
              static_cast<std::size_t>(engine->getExhaustSystemCount()), static_cast<std::size_t>(system->getRigidBodyCount()),
              static_cast<std::size_t>(system->getConstraintCount()), static_cast<std::size_t>(system->getFullConstraintCount()),
              static_cast<std::size_t>(system->getForceGeneratorCount())};
    const auto &expected = report.predicted;
    return actual.cylinders == expected.cylinders && actual.cranks == expected.cranks && actual.banks == expected.banks &&
        actual.intakes == expected.intakes && actual.exhausts == expected.exhausts && actual.bodies == expected.bodies &&
        actual.constraints + expected.reserved_rows == expected.constraints &&
        actual.rows + expected.reserved_rows == expected.rows && actual.generators + expected.reserved_generators == expected.generators &&
        CapacityInspection::compare(report, *simulator, built.synthesis);
}

}
