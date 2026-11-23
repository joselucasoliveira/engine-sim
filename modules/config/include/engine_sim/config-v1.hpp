#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace engine_sim::config_v1 {
template<class T>
struct Ref {
    std::string value;

    friend bool operator==(const Ref &a, const Ref &b) noexcept { return a.value == b.value; }
    friend bool operator!=(const Ref &a, const Ref &b) noexcept { return !(a == b); }
};

template<class T>
struct Measure { double value{}; };
struct CamshaftTag;
struct ConnectingRodTag;
struct CrankshaftTag;
struct CylinderBankTag;
struct EngineTag;
struct ExhaustSystemTag;
struct FuelTag;
struct FunctionTag;
struct HeadTag;
struct IgnitionTag;
struct ImpulseResponseTag;
struct IntakeTag;
struct PistonTag;
struct RodJournalTag;
struct ThrottleTag;
struct ValvetrainTag;
struct WireTag;
struct AxisXUnit;
struct AxisYUnit;
struct AngleUnit;
struct LengthUnit;
struct MassUnit;
struct TorqueUnit;
struct InertiaUnit;
struct FlowKUnit;
struct VolumeUnit;
struct MolarMassUnit;
struct SpecificEnergyUnit;
struct DensityUnit;
struct RatioUnit;
struct AngularSpeedUnit;
struct GovernorRateUnit;
struct GovernorSpringUnit;
struct FrequencyUnit;
struct GainUnit;
struct DisplayDepthUnit;
struct AreaUnit;
struct SpeedUnit;
struct PressureUnit;
struct TimeUnit;
struct ForceUnit;

struct Sample {
    Measure<AxisXUnit> x{};
    Measure<AxisYUnit> y{};
};

struct Function {
    Ref<FunctionTag> id{};
    Measure<AxisXUnit> filter_radius{};
    static constexpr auto input_scale = 1;
    static constexpr auto output_scale = 1;
    std::vector<Sample> samples{};
};

struct RodJournal {
    Ref<RodJournalTag> id{};
    Measure<AngleUnit> angle{};
};

struct Crankshaft {
    Ref<CrankshaftTag> id{};
    Measure<LengthUnit> crank_throw{};
    Measure<MassUnit> flywheel_mass{};
    Measure<MassUnit> mass{};
    Measure<TorqueUnit> friction_torque{};
    Measure<InertiaUnit> moment_of_inertia{};
    Measure<LengthUnit> position_x{};
    Measure<LengthUnit> position_y{};
    Measure<AngleUnit> tdc{};
    std::vector<Ref<RodJournalTag>> journals{};
};

struct Piston {
    Ref<PistonTag> id{};
    Measure<MassUnit> mass{};
    Measure<FlowKUnit> blowby{};
    Measure<LengthUnit> compression_height{};
    Measure<LengthUnit> wrist_pin_position{};
    Measure<VolumeUnit> displacement{};
};

struct ConnectingRod {
    Ref<ConnectingRodTag> id{};
    Measure<MassUnit> mass{};
    Measure<InertiaUnit> moment_of_inertia{};
    Measure<LengthUnit> center_of_mass{};
    Measure<LengthUnit> length{};
    Measure<LengthUnit> slave_throw{};
    std::vector<Ref<RodJournalTag>> journals{};
};

struct Fuel {
    Ref<FuelTag> id{};
    std::string name{};
    Measure<MolarMassUnit> molecular_mass{};
    Measure<SpecificEnergyUnit> energy_density{};
    Measure<DensityUnit> density{};
    Measure<RatioUnit> molecular_afr{};
    Ref<FunctionTag> turbulence_to_flame_speed_ratio{};
    Measure<RatioUnit> max_burning_efficiency{};
    Measure<RatioUnit> burning_efficiency_randomness{};
    Measure<RatioUnit> low_efficiency_attenuation{};
    Measure<SpeedUnit> max_turbulence_effect{};
    Measure<RatioUnit> max_dilution_effect{};
};

struct DirectThrottle {
    Ref<ThrottleTag> id{};
    static constexpr auto kind = "direct";
    Measure<RatioUnit> gamma{};
};

struct Governor {
    Ref<ThrottleTag> id{};
    static constexpr auto kind = "governor";
    Measure<AngularSpeedUnit> min_speed{};
    Measure<AngularSpeedUnit> max_speed{};
    Measure<GovernorRateUnit> min_v{};
    Measure<GovernorRateUnit> max_v{};
    Measure<GovernorSpringUnit> k_s{};
    Measure<FrequencyUnit> k_d{};
    Measure<RatioUnit> gamma{};
};

struct Engine {
    Ref<EngineTag> id{};
    std::string name{};
    Measure<AngularSpeedUnit> redline{};
    Measure<AngularSpeedUnit> starter_speed{};
    Measure<TorqueUnit> starter_torque{};
    Measure<AngularSpeedUnit> dyno_min_speed{};
    Measure<AngularSpeedUnit> dyno_max_speed{};
    Measure<AngularSpeedUnit> dyno_hold_step{};
    Ref<FuelTag> fuel{};
    Ref<ThrottleTag> throttle{};
    Measure<FrequencyUnit> simulation_frequency{};
    Measure<GainUnit> hf_gain{};
    Measure<GainUnit> jitter{};
    Measure<GainUnit> noise{};
    std::vector<Ref<CrankshaftTag>> crankshafts{};
    std::vector<Ref<CylinderBankTag>> banks{};
    Ref<IgnitionTag> ignition{};
    Ref<FunctionTag> mean_piston_speed_to_turbulence{};
};

struct Cylinder {
    Ref<PistonTag> piston{};
    Ref<ConnectingRodTag> connecting_rod{};
    Ref<RodJournalTag> rod_journal{};
    Ref<IntakeTag> intake{};
    Ref<ExhaustSystemTag> exhaust_system{};
    Ref<WireTag> ignition_wire{};
    Measure<LengthUnit> primary_length{};
    Measure<GainUnit> sound_attenuation{};
};

struct CylinderBank {
    Ref<CylinderBankTag> id{};
    Measure<AngleUnit> angle{};
    Measure<LengthUnit> bore{};
    Measure<LengthUnit> deck_height{};
    Measure<LengthUnit> position_x{};
    Measure<LengthUnit> position_y{};
    Measure<DisplayDepthUnit> display_depth{};
    std::vector<Cylinder> cylinders{};
    Ref<HeadTag> head{};
};

struct Head {
    Ref<HeadTag> id{};
    Measure<VolumeUnit> chamber_volume{};
    Measure<VolumeUnit> intake_runner_volume{};
    Measure<AreaUnit> intake_runner_cross_section_area{};
    Measure<VolumeUnit> exhaust_runner_volume{};
    Measure<AreaUnit> exhaust_runner_cross_section_area{};
    bool flip_display{};
    Ref<FunctionTag> intake_port_flow{};
    Ref<FunctionTag> exhaust_port_flow{};
    Ref<ValvetrainTag> valvetrain{};
};

struct Standard {
    Ref<ValvetrainTag> id{};
    static constexpr auto kind = "standard";
    Ref<CamshaftTag> intake_camshaft{};
    Ref<CamshaftTag> exhaust_camshaft{};
};

struct Vtec {
    Ref<ValvetrainTag> id{};
    static constexpr auto kind = "vtec";
    Ref<CamshaftTag> intake_camshaft{};
    Ref<CamshaftTag> exhaust_camshaft{};
    Ref<CamshaftTag> vtec_intake_camshaft{};
    Ref<CamshaftTag> vtec_exhaust_camshaft{};
    Measure<AngularSpeedUnit> min_rpm{};
    Measure<SpeedUnit> min_speed{};
    Measure<PressureUnit> manifold_vacuum{};
    Measure<RatioUnit> min_throttle_position{};
};

struct Camshaft {
    Ref<CamshaftTag> id{};
    Measure<AngleUnit> advance{};
    Measure<LengthUnit> base_radius{};
    Ref<FunctionTag> lobe_profile{};
    std::vector<Measure<AngleUnit>> lobes{};
};

struct Intake {
    Ref<IntakeTag> id{};
    Measure<VolumeUnit> plenum_volume{};
    Measure<AreaUnit> plenum_cross_section_area{};
    Measure<FlowKUnit> intake_flow_rate{};
    Measure<FlowKUnit> idle_flow_rate{};
    Measure<FlowKUnit> runner_flow_rate{};
    Measure<RatioUnit> molecular_afr{};
    Measure<RatioUnit> idle_throttle_plate_position{};
    Measure<LengthUnit> runner_length{};
    Measure<RatioUnit> velocity_decay{};
};

struct ExhaustSystem {
    Ref<ExhaustSystemTag> id{};
    Measure<LengthUnit> length{};
    Measure<AreaUnit> collector_cross_section_area{};
    Measure<FlowKUnit> outlet_flow_rate{};
    Measure<LengthUnit> primary_tube_length{};
    Measure<FlowKUnit> primary_flow_rate{};
    Measure<GainUnit> audio_volume{};
    Measure<RatioUnit> velocity_decay{};
    Ref<ImpulseResponseTag> impulse_response{};
};

struct ImpulseResponse {
    Ref<ImpulseResponseTag> id{};
    std::string path{};
    Measure<GainUnit> volume{};
};

struct Post {
    Ref<WireTag> wire{};
    Measure<AngleUnit> angle{};
};

struct Wire {
    Ref<WireTag> id{};
};

struct Ignition {
    Ref<IgnitionTag> id{};
    Ref<FunctionTag> timing_curve{};
    Measure<AngularSpeedUnit> rev_limit{};
    Measure<TimeUnit> limiter_duration{};
    std::vector<Post> posts{};
};

struct Vehicle {
    Measure<MassUnit> mass{};
    Measure<RatioUnit> drag_coefficient{};
    Measure<AreaUnit> cross_sectional_area{};
    Measure<RatioUnit> diff_ratio{};
    Measure<LengthUnit> tire_radius{};
    Measure<ForceUnit> rolling_resistance{};
};

struct Transmission {
    Measure<TorqueUnit> max_clutch_torque{};
    std::vector<Measure<RatioUnit>> gears{};
};

struct Desktop {
    bool start_fullscreen{};
    std::string power_units{};
    std::string torque_units{};
    std::string speed_units{};
    std::string pressure_units{};
    std::string boost_units{};
    std::int32_t color_background{};
    std::int32_t color_foreground{};
    std::int32_t color_shadow{};
    std::int32_t color_highlight1{};
    std::int32_t color_highlight2{};
    std::int32_t color_pink{};
    std::int32_t color_red{};
    std::int32_t color_orange{};
    std::int32_t color_yellow{};
    std::int32_t color_blue{};
    std::int32_t color_green{};
};

struct Initialization {
    static constexpr auto profile = "r2-piston-v1";
    static constexpr auto crankcase_pa = 101325;
    static constexpr auto chamber_pa = 101325;
    static constexpr auto chamber_kelvin = 298.15;
};

using Throttle = std::variant<DirectThrottle, Governor>;
using Valvetrain = std::variant<Standard, Vtec>;

struct Components {
    std::vector<Function> functions{};
    std::vector<RodJournal> journals{};
    std::vector<Crankshaft> crankshafts{};
    std::vector<Piston> pistons{};
    std::vector<ConnectingRod> rods{};
    std::vector<Fuel> fuels{};
    std::vector<Throttle> throttles{};
    std::vector<CylinderBank> banks{};
    std::vector<Head> heads{};
    std::vector<Valvetrain> valvetrains{};
    std::vector<Camshaft> camshafts{};
    std::vector<Intake> intakes{};
    std::vector<ExhaustSystem> exhausts{};
    std::vector<ImpulseResponse> impulses{};
    std::vector<Wire> wires{};
    std::vector<Ignition> ignitions{};
};

// Index records describe validated bank-major construction. Authored records
// carry typed IDs, including a journal ID whose owner determines its route.
struct CrankJournalIndex {
    std::size_t crankshaft{};
    std::size_t journal{};
};

struct MasterRodJournalIndex {
    std::size_t rod{};
    std::size_t journal{};
};

using JournalOwnerIndex = std::variant<CrankJournalIndex, MasterRodJournalIndex>;

struct CylinderIndex {
    std::size_t bank{};
    std::size_t local{};
    std::size_t global{};
    JournalOwnerIndex journal{};
};

// Each optional authored record has a complete value after defaults resolve.
struct ResolvedDefaults {
    Initialization initialization{};
    Vehicle vehicle{};
    Transmission transmission{};
    Desktop desktop{};
};

struct Document {
    static constexpr auto format_version = 1;
    Ref<EngineTag> selected_engine{};
    Engine engine{};
    Components components{};
    std::optional<Initialization> initialization{};
    std::optional<Vehicle> vehicle{};
    std::optional<Transmission> transmission{};
    std::optional<Desktop> desktop{};
};

struct Diagnostic {
    std::string code, stage, pointer, message;
    std::optional<std::string> component_id, field, path;
    std::optional<std::uint64_t> byte_offset, line, column;
};
class NormalizedDocument; // immutable owned state returned by the normalization API
} // namespace engine_sim::config_v1
