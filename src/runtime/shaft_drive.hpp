#ifndef ENGINE_SIM_PRIVATE_SHAFT_DRIVE_HPP
#define ENGINE_SIM_PRIVATE_SHAFT_DRIVE_HPP

#include "simulator.h"
#include "simulation_fault.h"
#include <algorithm>
#include <cmath>

namespace engine_sim {
// Session-local mechanics. The simulator and its model outlive this adapter.
class DrivenShaft final {
    struct Row final : atg_scs::Constraint {
        explicit Row(Crankshaft &crank) : Constraint(1, 1) { m_bodies[0] = &crank.m_body; }
        double rpm = 0;
        void calculate(Output *out, atg_scs::SystemState *) override {
            out->C[0] = 0;
            out->J[0][0] = out->J[0][1] = 0; out->J[0][2] = 1;
            out->J_dot[0][0] = out->J_dot[0][1] = out->J_dot[0][2] = 0;
            out->ks[0] = out->kd[0] = 0;
            out->v_bias[0] = units::rpm(rpm);
            out->limits[0][0] = -TorqueLimit * units::Nm;
            out->limits[0][1] = TorqueLimit * units::Nm;
        }
    };
    struct Load final : atg_scs::ForceGenerator {
        atg_scs::RigidBody *body = nullptr;
        double nm = 0;
        void apply(atg_scs::SystemState *state) override { state->t[body->index] += nm * units::Nm; }
    };
public:
    static constexpr double TorqueLimit = 1000000;
    static constexpr double SpeedToleranceRpm = .1;
    // Guards gross drift; ordinary numerical residuals remain model dependent.
    static constexpr double JointFraction = .003;
    explicit DrivenShaft(Simulator &sim) : m_sim(sim), m_crank(*sim.getEngine()->getOutputCrankshaft()), m_row(m_crank) {
        m_load.body = &m_crank.m_body;
        auto &system = *sim.getSystem();
        system.addForceGenerator(&m_load);
        try { system.addConstraint(&m_row); }
        catch (...) { system.removeForceGenerator(&m_load); throw; }
    }
    ~DrivenShaft() {
        m_sim.getSystem()->removeConstraint(&m_row);
        m_sim.getSystem()->removeForceGenerator(&m_load);
    }
    DrivenShaft(const DrivenShaft &) = delete;
    DrivenShaft &operator=(const DrivenShaft &) = delete;
    void latch(double rpm, double load) noexcept { m_row.rpm = rpm; m_load.nm = rpm > 0 ? load : 0; }
    void beginTick() noexcept { m_beforeAngle = m_crank.m_body.theta; }
    ProcessingStatus finishTick() noexcept {
        const double reaction = m_row.F_t[0][0] / units::Nm;
        if (!std::isfinite(reaction) || std::abs(reaction) >= TorqueLimit * (1 - 1e-12))
            return ProcessingStatus::drive_saturation;
        const double residual = units::toRpm(m_crank.m_body.v_theta) + m_row.rpm;
        if (!std::isfinite(residual) || std::abs(residual) > SpeedToleranceRpm)
            return ProcessingStatus::drive_speed;
        const double dt = m_sim.getTimestep();
        const double phase = std::remainder(m_crank.m_body.theta - m_beforeAngle + units::rpm(m_row.rpm) * dt, 4 * constants::pi);
        if (!std::isfinite(phase) || std::abs(phase) > units::rpm(SpeedToleranceRpm) * dt + 1e-10)
            return ProcessingStatus::drive_phase;
        if (const auto status = checkJoints(); status != ProcessingStatus::ok) return status;
        m_reaction = reaction; m_appliedRpm = m_row.rpm; m_loadReaction = m_load.nm;
        return ProcessingStatus::ok;
    }
    double reactionNm() const noexcept { return m_reaction; }
    double loadReactionNm() const noexcept { return m_loadReaction; }
    double appliedRpm() const noexcept { return m_appliedRpm; }
private:
    ProcessingStatus checkJoints() const noexcept {
        auto &engine = *m_sim.getEngine();
        double shortest = engine.getConnectingRod(0)->getLength();
        for (int i = 0; i < engine.getCylinderCount(); ++i) {
            auto &piston = *engine.getPiston(i); auto &rod = *piston.getRod(); auto &bank = *piston.getCylinderBank();
            double bx, by, sx, sy, wx, wy, jx, jy;
            rod.m_body.localToWorld(0, rod.getBigEndLocal(), &bx, &by);
            rod.m_body.localToWorld(0, rod.getLittleEndLocal(), &sx, &sy);
            piston.m_body.localToWorld(0, piston.getWristPinLocation(), &wx, &wy);
            if (rod.getMasterRod()) rod.getMasterRod()->getRodJournalPositionGlobal(rod.getJournal(), &jx, &jy);
            else {
                double lx, ly; rod.getCrankshaft()->getRodJournalPositionLocal(rod.getJournal(), &lx, &ly);
                rod.getCrankshaft()->m_body.localToWorld(lx, ly, &jx, &jy);
            }
            const double angle = bank.getAngle() + constants::pi / 2;
            const double wall = std::abs((wx - bank.getX()) * std::sin(angle) - (wy - bank.getY()) * std::cos(angle));
            for (double error : {std::hypot(bx - jx, by - jy), std::hypot(sx - wx, sy - wy), wall}) {
                if (!std::isfinite(error) || error > std::max(1e-9, JointFraction * rod.getLength()))
                    return ProcessingStatus::drive_joint;
            }
            shortest = std::min(shortest, rod.getLength());
        }
        for (int i = 0; i < engine.getCrankshaftCount(); ++i) {
            auto &crank = *engine.getCrankshaft(i);
            const double error = std::hypot(crank.m_body.p_x - crank.getPosX(), crank.m_body.p_y - crank.getPosY());
            if (!std::isfinite(error) || error > std::max(1e-9, JointFraction * shortest))
                return ProcessingStatus::drive_crank;
        }
        for (int i = 0; i < engine.getCylinderBankCount(); ++i) {
            auto &head = *engine.getHead(i);
            for (auto *cam : {head.getIntakeCamshaft(), head.getExhaustCamshaft()}) {
                if (!cam) return ProcessingStatus::drive_cam_missing;
                const double angle = cam->getAngle();
                if (!std::isfinite(angle) || angle < 0 || angle >= 2 * constants::pi)
                    return ProcessingStatus::drive_cam_phase;
            }
        }
        return ProcessingStatus::ok;
    }
    Simulator &m_sim;
    Crankshaft &m_crank;
    Row m_row;
    Load m_load;
    double m_beforeAngle = 0, m_reaction = 0, m_appliedRpm = 0, m_loadReaction = 0;
};
}
#endif
