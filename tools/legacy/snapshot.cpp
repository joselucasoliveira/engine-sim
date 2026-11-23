#include "snapshot.hpp"

#include "cylinder_head_node.h"
#include "engine_node.h"
#include "model_validation.h"
#include "standard_valvetrain_node.h"
#include "transmission_node.h"
#include "vehicle_node.h"
#include "vtec_valvetrain_node.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

namespace es_script {

namespace cfg = engine_sim::config_v1;
namespace legacy = engine_sim::legacy;

namespace {
using Wires = std::unordered_map<const CylinderBankNode *, std::vector<const IgnitionWireNode *>>;

std::string identifier(std::string_view kind, std::size_t ordinal) {
    auto digits = std::to_string(ordinal);
    std::string result(kind);
    result += '-';
    result.append(6 - digits.size(), '0');
    result += digits;
    return result;
}
}

class LegacySnapshotBuilder {
public:
    explicit LegacySnapshotBuilder(const Wires &wires) : m_wires(wires) {}

    static cfg::Vehicle vehicle(const VehicleNode &source) {
        const auto &p = source.m_parameters;
        return {{p.mass}, {p.dragCoefficient}, {p.crossSectionArea}, {p.diffRatio},
                {p.tireRadius}, {p.rollingResistance}};
    }

    static cfg::Transmission transmission(const TransmissionNode &source) {
        cfg::Transmission result;
        result.max_clutch_torque = {source.m_parameters.MaxClutchTorque};
        result.gears.reserve(source.m_gears.size());
        for (const auto ratio : source.m_gears) result.gears.push_back({ratio});
        return result;
    }


    legacy::Snapshot capture(const EngineNode &node) {
        auto &result = m_snapshot.values.engine;
        const auto &params = node.m_parameters;
        result.id = {"engine-000001"};
        m_snapshot.values.selected_engine = result.id;
        result.name = params.name;
        result.redline = {params.redline};
        result.starter_speed = {params.starterSpeed};
        result.starter_torque = {params.starterTorque};
        result.dyno_min_speed = {params.dynoMinSpeed};
        result.dyno_max_speed = {params.dynoMaxSpeed};
        result.dyno_hold_step = {params.dynoHoldStep};
        result.simulation_frequency = {params.initialSimulationFrequency};
        result.hf_gain = {params.initialHighFrequencyGain};
        result.jitter = {params.initialJitter};
        result.noise = {params.initialNoise};
        for (const auto *shaft : node.m_crankshafts) result.crankshafts.push_back(crankshaft(shaft));
        for (const auto *part : node.m_cylinderBanks) result.banks.push_back(bank(part));
        result.ignition = ignition(node.m_ignitionModule);
        result.fuel = fuel(node.m_fuel);
        result.throttle = throttle(node.m_throttle);
        result.mean_piston_speed_to_turbulence = meanSpeedCurve();
        return std::move(m_snapshot);
    }

private:
    template<class Ref, class Node, class Record, class Fill>
    Ref visit(const Node *source, std::string_view kind, std::vector<Record> &table, Fill &&fill) {
        requireModel(source != nullptr, "Missing required evaluated " + std::string(kind));
        const auto found = m_ids.find(source);
        if (found != m_ids.end()) return {found->second};

        // Include the root and the generated mean piston speed function.
        requireModel(m_ids.size() < 65'534, "Too many evaluated component definitions");
        const auto index = table.size();
        const Ref id{identifier(kind, index + 1)};
        m_ids.emplace(source, id.value);
        table.emplace_back();
        auto record = fill(*source, id);
        // Child discovery can grow a table. Publish by index after recursion.
        table.at(index) = std::move(record);
        return id;
    }

    template<class Node, class Record, class Fill>
    auto component(const Node *source, std::string_view kind, std::vector<Record> &table, Fill &&fill) {
        return visit<decltype(Record::id)>(source, kind, table, [&](const Node &node, const auto &id) {
            Record record;
            record.id = id;
            fill(node, record);
            return record;
        });
    }

    cfg::Components &parts() { return m_snapshot.values.components; }

    cfg::Ref<cfg::FunctionTag> function(const FunctionNode *source) {
        return component(source, "function", parts().functions, [](const auto &node, auto &record) {
            record.filter_radius = {node.m_filterRadius};
            record.samples.reserve(node.m_samples.size());
            for (const auto &sample : node.m_samples) record.samples.push_back({{sample.x}, {sample.y}});
        });
    }

    cfg::Ref<cfg::FunctionTag> meanSpeedCurve() {
        auto &table = parts().functions;
        cfg::Function curve;
        curve.id = {identifier("function", table.size() + 1)};
        curve.filter_radius = {1.0};
        curve.samples.reserve(30);
        for (int i = 0; i < 30; ++i) {
            const double speed = static_cast<double>(i);
            curve.samples.push_back({{speed}, {speed * 0.5}});
        }
        const auto id = curve.id;
        table.push_back(std::move(curve));
        return id;
    }

    cfg::Ref<cfg::RodJournalTag> journal(const RodJournalNode *source) {
        return component(source, "rod_journal", parts().journals, [](const auto &node, auto &record) {
            record.angle = {node.getAngle()};
        });
    }

    cfg::Ref<cfg::CrankshaftTag> crankshaft(const CrankshaftNode *source) {
        return component(source, "crankshaft", parts().crankshafts, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.crank_throw = {p.crankThrow};
            record.flywheel_mass = {p.flywheelMass};
            record.mass = {p.mass};
            record.friction_torque = {p.frictionTorque};
            record.moment_of_inertia = {p.momentOfInertia};
            record.position_x = {p.pos_x};
            record.position_y = {p.pos_y};
            record.tdc = {p.tdc};
            for (const auto *part : node.getJournals()) record.journals.push_back(journal(part));
        });
    }

    cfg::Ref<cfg::PistonTag> piston(const PistonNode *source) {
        return component(source, "piston", parts().pistons, [](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.mass = {p.mass};
            record.blowby = {p.BlowbyFlowCoefficient};
            record.compression_height = {p.CompressionHeight};
            record.wrist_pin_position = {p.WristPinPosition};
            record.displacement = {p.Displacement};
        });
    }

    cfg::Ref<cfg::ConnectingRodTag> rod(const ConnectingRodNode *source) {
        return component(source, "connecting_rod", parts().rods, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.mass = {p.mass};
            record.moment_of_inertia = {p.momentOfInertia};
            record.center_of_mass = {p.centerOfMass};
            record.length = {p.length};
            record.slave_throw = {p.slaveThrow};
            for (const auto *part : node.getJournals()) record.journals.push_back(journal(part));
        });
    }

    cfg::Ref<cfg::FuelTag> fuel(const FuelNode *source) {
        return component(source, "fuel", parts().fuels, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.name = p.name;
            record.molecular_mass = {p.molecularMass};
            record.energy_density = {p.energyDensity};
            record.density = {p.density};
            record.molecular_afr = {p.molecularAfr};
            record.turbulence_to_flame_speed_ratio = function(node.m_turbulenceToFlameSpeedRatio);
            record.max_burning_efficiency = {p.maxBurningEfficiency};
            record.burning_efficiency_randomness = {p.burningEfficiencyRandomness};
            record.low_efficiency_attenuation = {p.lowEfficiencyAttenuation};
            record.max_turbulence_effect = {p.maxTurbulenceEffect};
            record.max_dilution_effect = {p.maxDilutionEffect};
        });
    }

    cfg::Ref<cfg::ThrottleTag> throttle(const ThrottleNode *source) {
        return visit<cfg::Ref<cfg::ThrottleTag>>(source, "throttle", parts().throttles,
                [](const ThrottleNode &node, const auto &id) -> cfg::Throttle {
            if (const auto *direct = dynamic_cast<const DirectThrottleLinkageNode *>(&node)) {
                return cfg::DirectThrottle{id, {direct->m_parameters.gamma}};
            }
            if (const auto *governor = dynamic_cast<const GovernorNode *>(&node)) {
                const auto &p = governor->m_parameters;
                return cfg::Governor{id, {p.minSpeed}, {p.maxSpeed}, {p.minVelocity},
                    {p.maxVelocity}, {p.k_s}, {p.k_d}, {p.gamma}};
            }
            throw ModelError("Unsupported evaluated throttle alternative");
        });
    }

    cfg::Ref<cfg::IntakeTag> intake(const IntakeNode *source) {
        return component(source, "intake", parts().intakes, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.plenum_volume = {p.volume};
            record.plenum_cross_section_area = {p.CrossSectionArea};
            record.intake_flow_rate = {p.InputFlowK};
            record.idle_flow_rate = {p.IdleFlowK};
            record.runner_flow_rate = {p.RunnerFlowRate};
            record.molecular_afr = {p.MolecularAfr};
            record.idle_throttle_plate_position = {p.IdleThrottlePlatePosition};
            record.runner_length = {p.RunnerLength};
            record.velocity_decay = {p.VelocityDecay};
            m_snapshot.ignored_inputs.push_back({record.id.value, "throttle_gamma", node.m_throttleGammaUnused});
        });
    }

    cfg::Ref<cfg::ImpulseResponseTag> impulse(const ImpulseResponseNode *source) {
        return component(source, "impulse_response", parts().impulses, [&](const auto &node, auto &record) {
            requireModel(node.m_irStructure && node.m_irStructure->getParentUnit(),
                         "Missing declaring compilation unit for impulse response");
            const auto &unit = node.m_irStructure->getParentUnit()->getPath();
            record.path = node.m_filename;
            record.volume = {node.m_volume};
            m_snapshot.impulses.push_back({record.id, std::filesystem::u8path(unit.toString()), node.m_filename});
        });
    }

    cfg::Ref<cfg::ExhaustSystemTag> exhaust(const ExhaustSystemNode *source) {
        return component(source, "exhaust_system", parts().exhausts, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.length = {p.length};
            record.collector_cross_section_area = {p.collectorCrossSectionArea};
            record.outlet_flow_rate = {p.outletFlowRate};
            record.primary_tube_length = {p.primaryTubeLength};
            record.primary_flow_rate = {p.primaryFlowRate};
            record.audio_volume = {p.audioVolume};
            record.velocity_decay = {p.velocityDecay};
            record.impulse_response = impulse(node.m_impulseResponse);
        });
    }

    cfg::Ref<cfg::WireTag> wire(const IgnitionWireNode *source) {
        return component(source, "wire", parts().wires, [](const auto &, auto &) {});
    }

    cfg::Ref<cfg::CylinderBankTag> bank(const CylinderBankNode *source) {
        return component(source, "cylinder_bank", parts().banks, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.angle = {p.angle};
            record.bore = {p.bore};
            record.deck_height = {p.deckHeight};
            record.position_x = {p.positionX};
            record.position_y = {p.positionY};
            record.display_depth = {p.displayDepth};
            const int count = node.getCylinderCount();
            const auto observed = m_wires.find(&node);
            requireModel(observed != m_wires.end() && observed->second.size() == static_cast<std::size_t>(count),
                         "Missing evaluated cylinder wire assignment");
            record.cylinders.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                const auto &entry = node.getCylinder(i);
                const auto *connected = observed->second.at(static_cast<std::size_t>(i));
                const auto &connections = connected->getConnections();
                requireModel(std::any_of(connections.begin(), connections.end(), [&](const auto &connection) {
                    return connection.first == &node && connection.second == i;
                }), "Evaluated cylinder wire disagrees with the legacy connection");
                record.cylinders.push_back({
                    piston(entry.piston), rod(entry.rod), journal(entry.rodJournal), intake(entry.intake),
                    exhaust(entry.exhaust), wire(connected), {entry.primaryLength}, {entry.soundAttenuation}
                });
            }
            record.head = head(node.getCylinderHead());
        });
    }

    cfg::Ref<cfg::HeadTag> head(const CylinderHeadNode *source) {
        return component(source, "head", parts().heads, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.chamber_volume = {p.CombustionChamberVolume};
            record.intake_runner_volume = {p.IntakeRunnerVolume};
            record.intake_runner_cross_section_area = {p.IntakeRunnerCrossSectionArea};
            record.exhaust_runner_volume = {p.ExhaustRunnerVolume};
            record.exhaust_runner_cross_section_area = {p.ExhaustRunnerCrossSectionArea};
            record.flip_display = p.FlipDisplay;
            record.intake_port_flow = function(node.m_intakePortFlow);
            record.exhaust_port_flow = function(node.m_exhaustPortFlow);
            record.valvetrain = valvetrain(node.m_valvetrain);
        });
    }

    cfg::Ref<cfg::ValvetrainTag> valvetrain(const ValvetrainNode *source) {
        return visit<cfg::Ref<cfg::ValvetrainTag>>(source, "valvetrain", parts().valvetrains,
                [&](const ValvetrainNode &node, const auto &id) -> cfg::Valvetrain {
            if (const auto *basic = dynamic_cast<const StandardValvetrainNode *>(&node)) {
                return cfg::Standard{id, camshaft(basic->m_intakeCamshaft), camshaft(basic->m_exhaustCamshaft)};
            }
            if (const auto *vtec = dynamic_cast<const VtecValvetrainNode *>(&node)) {
                const auto &p = vtec->m_parameters;
                return cfg::Vtec{id, camshaft(vtec->m_intakeCamshaft), camshaft(vtec->m_exhaustCamshaft),
                    camshaft(vtec->m_vtecIntakeCamshaft), camshaft(vtec->m_vtecExhaustCamshaft),
                    {p.minRpm}, {p.minSpeed}, {p.manifoldVacuum}, {p.minThrottlePosition}};
            }
            throw ModelError("Unsupported evaluated valvetrain alternative");
        });
    }

    cfg::Ref<cfg::CamshaftTag> camshaft(const CamshaftNode *source) {
        return component(source, "camshaft", parts().camshafts, [&](const auto &node, auto &record) {
            const auto &p = node.m_parameters;
            record.advance = {p.advance};
            record.base_radius = {p.baseRadius};
            record.lobe_profile = function(node.m_lobeProfile);
            for (const auto angle : node.m_lobes) record.lobes.push_back({angle});
        });
    }

    cfg::Ref<cfg::IgnitionTag> ignition(const IgnitionModuleNode *source) {
        return component(source, "ignition", parts().ignitions, [&](const auto &node, auto &record) {
            record.timing_curve = function(node.m_timingCurve);
            record.rev_limit = {node.m_revLimit};
            record.limiter_duration = {node.m_limiterDuration};
            for (const auto &post : node.getPosts()) record.posts.push_back({wire(post.wire), {post.angle}});
        });
    }

    const Wires &m_wires;
    std::unordered_map<const void *, std::string> m_ids;
    legacy::Snapshot m_snapshot;
};

namespace {
class Recorder final : public EvaluationObserver {
public:
    void selectedEngine(const EngineNode &engine) override {
        m_latest = LegacySnapshotBuilder(m_wires).capture(engine);
    }

    void addedCylinder(const CylinderBankNode &bank, const IgnitionWireNode &wire) override {
        auto &wires = m_wires[&bank];
        requireModel(wires.size() + 1 == static_cast<std::size_t>(bank.getCylinderCount()),
                     "Legacy cylinder append order is inconsistent");
        wires.push_back(&wire);
    }

    void selectedVehicle(const VehicleNode &vehicle) override {
        m_vehicle = LegacySnapshotBuilder::vehicle(vehicle);
    }

    void selectedTransmission(const TransmissionNode &transmission) override {
        m_transmission = LegacySnapshotBuilder::transmission(transmission);
    }

    void selectedSettings(const ApplicationSettings &settings) override {
        cfg::Desktop desktop;
        desktop.start_fullscreen = settings.startFullscreen;
        desktop.power_units = settings.powerUnits;
        desktop.torque_units = settings.torqueUnits;
        desktop.speed_units = settings.speedUnits;
        desktop.pressure_units = settings.pressureUnits;
        desktop.boost_units = settings.boostUnits;
        desktop.color_background = settings.colorBackground;
        desktop.color_foreground = settings.colorForeground;
        desktop.color_shadow = settings.colorShadow;
        desktop.color_highlight1 = settings.colorHighlight1;
        desktop.color_highlight2 = settings.colorHighlight2;
        desktop.color_pink = settings.colorPink;
        desktop.color_red = settings.colorRed;
        desktop.color_orange = settings.colorOrange;
        desktop.color_yellow = settings.colorYellow;
        desktop.color_blue = settings.colorBlue;
        desktop.color_green = settings.colorGreen;
        m_desktop = std::move(desktop);
    }

    legacy::Snapshot take() {
        requireModel(m_latest.has_value(), "No evaluated engine snapshot");
        auto current = std::move(*m_latest);
        current.values.vehicle = std::move(m_vehicle);
        current.values.transmission = std::move(m_transmission);
        current.values.desktop = std::move(m_desktop);
        return current;
    }

private:
    Wires m_wires;
    std::optional<legacy::Snapshot> m_latest;
    std::optional<cfg::Vehicle> m_vehicle;
    std::optional<cfg::Transmission> m_transmission;
    std::optional<cfg::Desktop> m_desktop;
};
}

legacy::Snapshot evaluateSnapshot(const PreparationContext &source, const Compiler::InputVerifier &verify) {
    Recorder recorder;
    std::vector<ScriptInput> consumed;
    // Finish legacy execution and release its live engine and compiler. An
    // execution or construction failure cannot publish a partial snapshot.
    (void)Compiler::prepare(source, consumed, verify, &recorder);
    auto result = recorder.take();
    result.consumed_scripts = std::move(consumed);
    return result;
}

}

engine_sim::legacy::Snapshot engine_sim::legacy::capture(const es_script::PreparationContext &source,
        const es_script::Compiler::InputVerifier &verify) {
    return es_script::evaluateSnapshot(source, verify);
}
