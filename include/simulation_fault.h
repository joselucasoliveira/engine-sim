#ifndef ENGINE_SIM_SIMULATION_FAULT_H
#define ENGINE_SIM_SIMULATION_FAULT_H

#include <cstdint>
#include <stdexcept>

namespace engine_sim {
class SimulationFault : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

enum class ProcessingStatus : std::uint8_t {
    ok, clock_faulted, clock_span, clock_capacity, render_capacity,
    physics_no_progress, mechanical_state, crank_cycle, torque_history,
    chamber_force, solver_convergence, exhaust_endpoint, interpolated_exhaust,
    native_no_progress, native_unprepared, native_span, native_input, native_filter,
    native_channel, native_mix, quantization,
    drive_saturation, drive_speed, drive_phase, drive_joint, drive_crank, drive_cam_missing, drive_cam_phase,
    load_reaction, telemetry_nonfinite
};

constexpr const char *processingMessage(ProcessingStatus failure) noexcept {
    switch (failure) {
    case ProcessingStatus::ok: return "";
    case ProcessingStatus::clock_faulted: return "sample clock requires recovery after a fault";
    case ProcessingStatus::clock_span: return "invalid sample clock output span";
    case ProcessingStatus::clock_capacity: return "sample clock counter capacity exceeded";
    case ProcessingStatus::render_capacity: return "render: counter capacity exceeded";
    case ProcessingStatus::physics_no_progress: return "physics made no progress";
    case ProcessingStatus::mechanical_state: return "nonfinite mechanical state";
    case ProcessingStatus::crank_cycle: return "invalid crank cycle angle";
    case ProcessingStatus::torque_history: return "crank cycle index outside torque history";
    case ProcessingStatus::chamber_force: return "nonfinite combustion chamber force";
    case ProcessingStatus::solver_convergence: return "constraint solver did not converge";
    case ProcessingStatus::exhaust_endpoint: return "nonfinite exhaust endpoint";
    case ProcessingStatus::interpolated_exhaust: return "nonfinite interpolated exhaust input";
    case ProcessingStatus::native_no_progress: return "native synthesis made no progress";
    case ProcessingStatus::native_unprepared: return "Synchronous synthesis is not prepared";
    case ProcessingStatus::native_span: return "Native synthesis requires one input per prepared channel";
    case ProcessingStatus::native_input: return "nonfinite native synthesis input";
    case ProcessingStatus::native_filter: return "nonfinite interpolated synthesis input";
    case ProcessingStatus::native_channel: return "nonfinite synthesis channel";
    case ProcessingStatus::native_mix: return "nonfinite mixed synthesis signal";
    case ProcessingStatus::quantization: return "nonfinite signal before quantization";
    case ProcessingStatus::drive_saturation: return "driven shaft torque authority saturated";
    case ProcessingStatus::drive_speed: return "driven shaft signed speed residual exceeds 0.1 RPM";
    case ProcessingStatus::drive_phase: return "driven shaft integrated phase is inconsistent";
    case ProcessingStatus::drive_joint: return "driven mechanism joint residual exceeds geometry budget";
    case ProcessingStatus::drive_crank: return "driven crank center exceeds geometry budget";
    case ProcessingStatus::drive_cam_missing: return "driven cam is missing";
    case ProcessingStatus::drive_cam_phase: return "driven cam phase is invalid";
    case ProcessingStatus::load_reaction: return "nonfinite external load reaction";
    case ProcessingStatus::telemetry_nonfinite: return "nonfinite runtime telemetry";
    }
    return "unexpected processing failure";
}

// The legacy C++ callers keep their exception contract at the outer entry.
inline void requireProcessing(ProcessingStatus status) {
    if (status == ProcessingStatus::ok) return;
    const auto *message = processingMessage(status);
    switch (status) {
    case ProcessingStatus::clock_capacity:
    case ProcessingStatus::render_capacity: throw std::overflow_error(message);
    case ProcessingStatus::clock_span:
    case ProcessingStatus::native_span: throw std::invalid_argument(message);
    case ProcessingStatus::native_unprepared: throw std::logic_error(message);
    default: throw SimulationFault(message);
    }
}

}
#endif
