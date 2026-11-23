#include "definition.hpp"
#include "mechanical.hpp"

#include "constants.h"
#include "direct_throttle_linkage.h"
#include "governor.h"
#include "standard_valvetrain.h"
#include "vtec_valvetrain.h"

#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace engine_sim::assembly {
namespace {
namespace cfg = config_v1;

template<class Record>
std::string_view key(const Record &record) {
    return record.id.value;
}

template<class... Records>
std::string_view key(const std::variant<Records...> &record) {
    return std::visit([](const auto &value) { return key(value); }, record);
}

template<class Record>
class Table {
public:
    explicit Table(const std::vector<Record> &records) : m_records(records) {
        m_index.reserve(records.size());
        for (std::size_t i = 0; i < records.size(); ++i)
            m_index.emplace(key(records[i]), i);
    }

    template<class Tag>
    std::size_t index(const cfg::Ref<Tag> &id) const { return m_index.at(id.value); }

    template<class Tag>
    const Record &get(const cfg::Ref<Tag> &id) const { return m_records[index(id)]; }

private:
    const std::vector<Record> &m_records;
    std::unordered_map<std::string_view, std::size_t> m_index;
};

std::unique_ptr<Throttle> make_throttle(const cfg::DirectThrottle &source) {
    auto throttle = std::make_unique<DirectThrottleLinkage>();
    throttle->initialize({source.gamma.value});
    return throttle;
}

std::unique_ptr<Throttle> make_throttle(const cfg::Governor &source) {
    auto throttle = std::make_unique<Governor>();
    Governor::Parameters p{};
    p.minSpeed = source.min_speed.value;
    p.maxSpeed = source.max_speed.value;
    p.minVelocity = source.min_v.value;
    p.maxVelocity = source.max_v.value;
    p.k_s = source.k_s.value;
    p.k_d = source.k_d.value;
    p.gamma = source.gamma.value;
    throttle->initialize(p);
    return throttle;
}

class Builder {
public:
    explicit Builder(const cfg::NormalizedDocument &model)
        : m_values(model.values()), m_topology(model.topology()),
          m_function_definitions(m_values.components.functions), m_heads(m_values.components.heads),
          m_trains(m_values.components.valvetrains), m_camshafts(m_values.components.camshafts),
          m_impulses(m_values.components.impulses), m_functions(m_values.components.functions.size()) {}

    std::unique_ptr<Throttle> throttle() const {
        const Table<cfg::Throttle> definitions(m_values.components.throttles);
        return std::visit([](const auto &source) { return make_throttle(source); },
                          definitions.get(m_values.engine.throttle));
    }

    void bank_ready(Engine &engine, std::size_t bank_index) {
        const auto &source = m_values.components.banks[m_topology.banks[bank_index]];
        auto &head = *engine.getHead(static_cast<int>(bank_index));
        assemble_head(engine, head, *engine.getCylinderBank(static_cast<int>(bank_index)),
                      m_heads.get(source.head));
        for (std::size_t i = 0; i < source.cylinders.size(); ++i, ++m_next_cylinder) {
            const auto &route = m_topology.cylinders[m_next_cylinder];
            const auto &cylinder = source.cylinders[i];
            auto *exhaust = engine.getExhaustSystem(static_cast<int>(route.exhaust));
            auto *intake = engine.getIntake(static_cast<int>(route.intake));
            // R2 initializes shared slots on each cylinder encounter. Every
            // exhaust initialization also owns its own response descriptor.
            assemble_exhaust(engine, *exhaust, m_values.components.exhausts[m_topology.exhausts[route.exhaust]]);
            assemble_intake(*intake, m_values.components.intakes[m_topology.intakes[route.intake]]);
            const auto local = static_cast<int>(i);
            head.setIntake(local, intake);
            head.setExhaustSystem(local, exhaust);
            head.setSoundAttenuation(local, cylinder.sound_attenuation.value);
            head.setHeaderPrimaryLength(local, cylinder.primary_length.value);
        }
    }

    void complete_engine(Engine &engine) {
        assemble_ignition(engine);
        auto *mean = function(engine, m_values.engine.mean_piston_speed_to_turbulence);
        assemble_fuel(engine);
        CombustionChamber::Parameters p{};
        p.CrankcasePressure = cfg::Initialization::crankcase_pa;
        p.Fuel = engine.getFuel();
        p.StartingPressure = cfg::Initialization::chamber_pa;
        p.StartingTemperature = cfg::Initialization::chamber_kelvin;
        p.MeanPistonSpeedToTurbulence = mean;
        for (int i = 0; i < engine.getCylinderCount(); ++i) {
            p.Piston = engine.getPiston(i);
            p.Head = engine.getHead(p.Piston->getCylinderBank()->getIndex());
            engine.getChamber(i)->initialize(p);
        }
    }

private:
    Function *function(Engine &engine, const cfg::Ref<cfg::FunctionTag> &ref) {
        const auto index = m_function_definitions.index(ref);
        auto *&function = m_functions[index];
        if (function) return function;
        const auto &source = m_values.components.functions[index];
        function = engine.createResource<Function>();
        function->initialize(static_cast<int>(source.samples.size()), source.filter_radius.value);
        // Function's ordered insertion determines R2's equal-x behavior.
        for (const auto &sample : source.samples) function->addSample(sample.x.value, sample.y.value);
        return function;
    }

    void initialize_camshaft(Engine &engine, Camshaft &cam, const cfg::Ref<cfg::CamshaftTag> &ref) {
        const auto &source = m_camshafts.get(ref);
        Camshaft::Parameters p{};
        p.lobes = static_cast<int>(source.lobes.size());
        p.advance = source.advance.value;
        p.crankshaft = engine.getCrankshaft(0);
        p.lobeProfile = function(engine, source.lobe_profile);
        p.baseRadius = source.base_radius.value;
        cam.initialize(p);
        for (int i = 0; i < p.lobes; ++i)
            cam.setLobeCenterline(i, source.lobes[static_cast<std::size_t>(i)].value);
    }

    Valvetrain *valvetrain(Engine &engine, const cfg::Standard &source) {
        auto *train = engine.createResource<StandardValvetrain>();
        auto *intake = engine.createResource<Camshaft>();
        auto *exhaust = engine.createResource<Camshaft>();
        initialize_camshaft(engine, *intake, source.intake_camshaft);
        initialize_camshaft(engine, *exhaust, source.exhaust_camshaft);
        train->initialize({intake, exhaust});
        return train;
    }

    Valvetrain *valvetrain(Engine &engine, const cfg::Vtec &source) {
        auto *train = engine.createResource<VtecValvetrain>();
        VtecValvetrain::Parameters p{};
        p.intakeCamshaft = engine.createResource<Camshaft>();
        p.exhaustCamshaft = engine.createResource<Camshaft>();
        p.vtecIntakeCamshaft = engine.createResource<Camshaft>();
        p.vtexExhaustCamshaft = engine.createResource<Camshaft>();
        initialize_camshaft(engine, *p.intakeCamshaft, source.intake_camshaft);
        initialize_camshaft(engine, *p.exhaustCamshaft, source.exhaust_camshaft);
        initialize_camshaft(engine, *p.vtecIntakeCamshaft, source.vtec_intake_camshaft);
        initialize_camshaft(engine, *p.vtexExhaustCamshaft, source.vtec_exhaust_camshaft);
        p.minRpm = source.min_rpm.value;
        p.minSpeed = source.min_speed.value;
        p.manifoldVacuum = source.manifold_vacuum.value;
        p.minThrottlePosition = source.min_throttle_position.value;
        p.engine = &engine;
        train->initialize(p);
        return train;
    }

    void assemble_head(Engine &engine, CylinderHead &head, CylinderBank &bank, const cfg::Head &source) {
        CylinderHead::Parameters p{};
        p.Bank = &bank;
        p.Valvetrain = std::visit([&](const auto &train) { return valvetrain(engine, train); },
                                  m_trains.get(source.valvetrain));
        p.IntakePortFlow = function(engine, source.intake_port_flow);
        p.ExhaustPortFlow = function(engine, source.exhaust_port_flow);
        p.CombustionChamberVolume = source.chamber_volume.value;
        p.IntakeRunnerVolume = source.intake_runner_volume.value;
        p.IntakeRunnerCrossSectionArea = source.intake_runner_cross_section_area.value;
        p.ExhaustRunnerVolume = source.exhaust_runner_volume.value;
        p.ExhaustRunnerCrossSectionArea = source.exhaust_runner_cross_section_area.value;
        p.FlipDisplay = source.flip_display;
        head.initialize(p);
    }

    void assemble_exhaust(Engine &engine, ExhaustSystem &exhaust, const cfg::ExhaustSystem &source) {
        const auto &response = m_impulses.get(source.impulse_response);
        auto *impulse = engine.createResource<ImpulseResponse>();
        impulse->initialize(response.path, response.volume.value);
        ExhaustSystem::Parameters p{};
        p.length = source.length.value;
        p.collectorCrossSectionArea = source.collector_cross_section_area.value;
        p.outletFlowRate = source.outlet_flow_rate.value;
        p.primaryTubeLength = source.primary_tube_length.value;
        p.primaryFlowRate = source.primary_flow_rate.value;
        p.velocityDecay = source.velocity_decay.value;
        p.audioVolume = source.audio_volume.value;
        p.impulseResponse = impulse;
        exhaust.initialize(p);
    }

    static void assemble_intake(Intake &intake, const cfg::Intake &source) {
        Intake::Parameters p{};
        p.volume = source.plenum_volume.value;
        p.CrossSectionArea = source.plenum_cross_section_area.value;
        p.InputFlowK = source.intake_flow_rate.value;
        p.IdleFlowK = source.idle_flow_rate.value;
        p.RunnerFlowRate = source.runner_flow_rate.value;
        p.MolecularAfr = source.molecular_afr.value;
        p.IdleThrottlePlatePosition = source.idle_throttle_plate_position.value;
        p.RunnerLength = source.runner_length.value;
        p.VelocityDecay = source.velocity_decay.value;
        intake.initialize(p);
    }

    void assemble_ignition(Engine &engine) {
        const Table<cfg::Ignition> definitions(m_values.components.ignitions);
        const Table<cfg::Wire> wires(m_values.components.wires);
        const auto &source = definitions.get(m_values.engine.ignition);
        IgnitionModule::Parameters p{};
        p.crankshaft = engine.getCrankshaft(0);
        p.cylinderCount = engine.getCylinderCount();
        p.revLimit = source.rev_limit.value;
        p.timingCurve = function(engine, source.timing_curve);
        p.limiterDuration = source.limiter_duration.value;
        auto &ignition = *engine.getIgnitionModule();
        ignition.initialize(p);
        for (const auto &post : source.posts) {
            for (const auto cylinder : m_topology.wire_cylinders[wires.index(post.wire)])
                ignition.setFiringOrder(static_cast<int>(cylinder), post.angle.value);
        }
    }

    void assemble_fuel(Engine &engine) {
        const Table<cfg::Fuel> definitions(m_values.components.fuels);
        const auto &source = definitions.get(m_values.engine.fuel);
        Fuel::Parameters p{};
        p.name = source.name;
        p.molecularMass = source.molecular_mass.value;
        p.energyDensity = source.energy_density.value;
        p.density = source.density.value;
        p.molecularAfr = source.molecular_afr.value;
        p.burningEfficiencyRandomness = source.burning_efficiency_randomness.value;
        p.lowEfficiencyAttenuation = source.low_efficiency_attenuation.value;
        p.maxBurningEfficiency = source.max_burning_efficiency.value;
        p.maxTurbulenceEffect = source.max_turbulence_effect.value;
        p.maxDilutionEffect = source.max_dilution_effect.value;
        p.turbulenceToFlameSpeedRatio = function(engine, source.turbulence_to_flame_speed_ratio);
        engine.getFuel()->initialize(p);
    }

    const cfg::Document &m_values;
    const cfg::ResolvedTopology &m_topology;
    Table<cfg::Function> m_function_definitions;
    Table<cfg::Head> m_heads;
    Table<cfg::Valvetrain> m_trains;
    Table<cfg::Camshaft> m_camshafts;
    Table<cfg::ImpulseResponse> m_impulses;
    std::vector<Function *> m_functions;
    std::size_t m_next_cylinder = 0;
};

std::string geometry_error(Engine &engine) {
    if (engine.getCylinderCount() > std::numeric_limits<int>::max() / 8)
        return "cylinder count overflow";
    for (int i = 0; i < engine.getCylinderCount(); ++i) {
        const auto &piston = *engine.getPiston(i);
        const auto &rod = *piston.getRod();
        const auto &bank = *piston.getCylinderBank();
        const auto &head = *engine.getHead(bank.getIndex());
        const int local = piston.getCylinderIndex();
        const double delay = (head.getHeaderPrimaryLength(local) + head.getExhaustSystem(local)->getLength())
            * 10000.0 / 343.0;
        if (!std::isfinite(delay) || delay <= 0 || delay >= std::numeric_limits<int>::max() - 32.0)
            return "exhaust delay extent";
        for (int sample = 0; sample < 1000; ++sample) {
            const double angle = 2 * constants::pi * sample / 1000;
            double x, y, orientation, travel;
            if (!placeRod(rod, bank, *rod.getCrankshaft(), angle, &x, &y, &orientation, &travel))
                return "cylinder " + std::to_string(i) + " has unusable rod geometry";
            const double volume = bank.boreSurfaceArea() *
                (bank.getDeckHeight() - travel - piston.getCompressionHeight()) +
                head.getCombustionChamberVolume() - piston.getDisplacement();
            if (!std::isfinite(volume) || volume <= 0)
                return "cylinder " + std::to_string(i) + " has nonpositive chamber volume";
        }
    }
    return {};
}

void assemble_drivetrain(EngineDefinition &result, const cfg::Vehicle &vehicle, const cfg::Transmission &transmission) {
    Vehicle::Parameters p{};
    p.mass = vehicle.mass.value;
    p.dragCoefficient = vehicle.drag_coefficient.value;
    p.crossSectionArea = vehicle.cross_sectional_area.value;
    p.diffRatio = vehicle.diff_ratio.value;
    p.tireRadius = vehicle.tire_radius.value;
    p.rollingResistance = vehicle.rolling_resistance.value;
    result.vehicle = std::make_unique<Vehicle>();
    result.vehicle->initialize(p);

    std::vector<double> ratios;
    ratios.reserve(transmission.gears.size());
    for (const auto ratio : transmission.gears) ratios.push_back(ratio.value);
    result.transmission = std::make_unique<Transmission>();
    result.transmission->initialize({static_cast<int>(ratios.size()), ratios.data(), transmission.max_clutch_torque.value});
}

ApplicationSettings desktop_settings(const cfg::Desktop &source) {
    ApplicationSettings settings;
    settings.startFullscreen = source.start_fullscreen;
    settings.powerUnits = source.power_units;
    settings.torqueUnits = source.torque_units;
    settings.speedUnits = source.speed_units;
    settings.pressureUnits = source.pressure_units;
    settings.boostUnits = source.boost_units;
    settings.colorBackground = source.color_background;
    settings.colorForeground = source.color_foreground;
    settings.colorShadow = source.color_shadow;
    settings.colorHighlight1 = source.color_highlight1;
    settings.colorHighlight2 = source.color_highlight2;
    settings.colorPink = source.color_pink;
    settings.colorRed = source.color_red;
    settings.colorOrange = source.color_orange;
    settings.colorYellow = source.color_yellow;
    settings.colorBlue = source.color_blue;
    settings.colorGreen = source.color_green;
    return settings;
}

}

DefinitionResult build_definition(const cfg::NormalizedDocument &model, std::uint64_t seed, std::string source) {
    Builder builder(model);
    MechanicalBuildOptions options;
    options.throttle = builder.throttle();
    options.bank_ready = [&](Engine &engine, std::size_t bank) { builder.bank_ready(engine, bank); };
    EngineDefinition definition;
    definition.sourcePath = std::move(source);
    definition.engine = assemble_mechanics(model, std::move(options));
    builder.complete_engine(*definition.engine);
    definition.engine->setRandomSeed(seed);
    auto failure = geometry_error(*definition.engine);
    if (!failure.empty()) {
        cfg::Diagnostic problem;
        problem.code = "CFG_INITIALIZE";
        problem.stage = "initialize";
        problem.pointer = "/engine";
        problem.message = std::move(failure);
        return problem;
    }
    const auto &values = model.values();
    assemble_drivetrain(definition, *values.vehicle, *values.transmission);
    definition.applicationSettings = desktop_settings(*values.desktop);
    return definition;
}

}
