#include "mechanical.hpp"

#include "connecting_rod.h"
#include "constants.h"
#include "crankshaft.h"
#include "cylinder_bank.h"
#include "piston.h"

#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace engine_sim::assembly {
namespace {
namespace cfg = config_v1;

int index(std::size_t count) {
    if (count > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::logic_error("Validated mechanical index exceeds legacy storage");
    return static_cast<int>(count);
}

class Builder {
public:
    Builder(const cfg::NormalizedDocument &model, const MechanicalCheckpoint &checkpoint)
        : m_document(model.values()), m_topology(model.topology()), m_checkpoint(checkpoint) {
        const auto &definitions=m_document.components.journals;
        m_journals.reserve(definitions.size());
        for (const auto &journal:definitions) m_journals.emplace(journal.id.value, &journal);
    }

    std::unique_ptr<Engine> build() const {
        auto engine=std::make_unique<Engine>();
        allocate(*engine);
        checkpoint(MechanicalPhase::engine, 0);
        crankshafts(*engine);
        banks(*engine);
        cylinders(*engine);
        connect_rods(*engine);
        checkpoint(MechanicalPhase::links, 0);
        return engine;
    }

private:
    void checkpoint(MechanicalPhase phase, std::size_t item) const {
        if (m_checkpoint) m_checkpoint(phase,item);
    }

    double source_angle(const cfg::Ref<cfg::RodJournalTag> &id) const {
        return m_journals.at(id.value)->angle.value;
    }

    void allocate(Engine &engine) const {
        const auto &source=m_document.engine;
        Engine::Parameters values{};
        values.crankshaftCount=index(m_topology.crankshafts.size());
        values.cylinderBanks=index(m_topology.banks.size());
        values.cylinderCount=index(m_topology.cylinders.size());
        values.exhaustSystemCount=index(m_topology.exhausts.size());
        values.intakeCount=index(m_topology.intakes.size());
        values.name=source.name;
        values.starterTorque=source.starter_torque.value;
        values.starterSpeed=source.starter_speed.value;
        values.redline=source.redline.value;
        values.dynoMinSpeed=source.dyno_min_speed.value;
        values.dynoMaxSpeed=source.dyno_max_speed.value;
        values.dynoHoldStep=source.dyno_hold_step.value;
        values.initialSimulationFrequency=source.simulation_frequency.value;
        values.initialHighFrequencyGain=source.hf_gain.value;
        values.initialJitter=source.jitter.value;
        values.initialNoise=source.noise.value;
        engine.initialize(values);
    }

    void crankshafts(Engine &engine) const {
        for (std::size_t i=0;i<m_topology.crankshafts.size();++i) {
            const auto &source=m_document.components.crankshafts.at(m_topology.crankshafts[i]);
            Crankshaft::Parameters values{};
            values.mass=source.mass.value;
            values.flywheelMass=source.flywheel_mass.value;
            values.momentOfInertia=source.moment_of_inertia.value;
            values.crankThrow=source.crank_throw.value;
            values.pos_x=source.position_x.value;
            values.pos_y=source.position_y.value;
            values.tdc=source.tdc.value;
            values.frictionTorque=source.friction_torque.value;
            values.rodJournals=index(source.journals.size());
            auto &shaft=*engine.getCrankshaft(index(i));
            shaft.initialize(values);
            for (std::size_t j=0;j<source.journals.size();++j)
                shaft.setRodJournalAngle(index(j),source_angle(source.journals[j]));
            checkpoint(MechanicalPhase::crankshaft,i);
        }
    }

    void banks(Engine &engine) const {
        for (std::size_t i=0;i<m_topology.banks.size();++i) {
            const auto &source=m_document.components.banks.at(m_topology.banks[i]);
            CylinderBank::Parameters values{};
            // R2 positions every bank relative to crank zero. Individual
            // cylinders retain their own validated crank and master journal.
            values.crankshaft=engine.getCrankshaft(0);
            values.positionX=source.position_x.value;
            values.positionY=source.position_y.value;
            values.angle=source.angle.value;
            values.bore=source.bore.value;
            values.deckHeight=source.deck_height.value;
            values.displayDepth=source.display_depth.value;
            values.cylinderCount=index(source.cylinders.size());
            values.index=index(i);
            engine.getCylinderBank(index(i))->initialize(values);
            checkpoint(MechanicalPhase::bank,i);
        }
    }

    void cylinders(Engine &engine) const {
        // Normalization has already preindexed every crank and slave journal
        // for the selected, bank-major cylinder stream.
        for (const auto &route:m_topology.cylinders) {
            const auto &part=route.index;
            auto *piston=engine.getPiston(index(part.global));
            auto *rod=engine.getConnectingRod(index(part.global));
            const auto &original_piston=m_document.components.pistons.at(route.piston_definition);
            Piston::Parameters piston_values{};
            piston_values.Rod=rod;
            piston_values.Bank=engine.getCylinderBank(index(part.bank));
            piston_values.CylinderIndex=index(part.local);
            piston_values.BlowbyFlowCoefficient=original_piston.blowby.value;
            piston_values.CompressionHeight=original_piston.compression_height.value;
            piston_values.WristPinPosition=original_piston.wrist_pin_position.value;
            piston_values.Displacement=original_piston.displacement.value;
            piston_values.mass=original_piston.mass.value;
            piston->initialize(piston_values);

            const auto &source=m_document.components.rods.at(route.rod_definition);
            ConnectingRod::Parameters values{};
            values.mass=source.mass.value;
            values.momentOfInertia=source.moment_of_inertia.value;
            values.centerOfMass=source.center_of_mass.value;
            values.length=source.length.value;
            values.rodJournals=index(source.journals.size());
            values.slaveThrow=source.slave_throw.value;
            values.piston=piston;
            if (const auto *direct=std::get_if<cfg::CrankJournalIndex>(&part.journal))
                values.crankshaft=engine.getCrankshaft(index(direct->crankshaft));
            values.journal=std::visit([](const auto &owner) { return index(owner.journal); },part.journal);
            rod->initialize(values);
            for (std::size_t j=0;j<source.journals.size();++j)
                rod->setRodJournalAngle(index(j),source_angle(source.journals[j])+constants::pi/2);
            checkpoint(MechanicalPhase::cylinder,part.global);
        }
    }

    void connect_rods(Engine &engine) const {
        for (const auto &route:m_topology.cylinders) {
            const auto &part=route.index;
            auto *rod=engine.getConnectingRod(index(part.global));
            if (const auto *master=std::get_if<cfg::MasterRodJournalIndex>(&part.journal))
                rod->setMaster(engine.getConnectingRod(index(master->rod)));
            rod->setCrankshaft(engine.getCrankshaft(index(route.root_crank)));
        }
    }

    const cfg::Document &m_document;
    const cfg::ResolvedTopology &m_topology;
    const MechanicalCheckpoint &m_checkpoint;
    std::unordered_map<std::string_view,const cfg::RodJournal *> m_journals;
};
}

MechanicalResult build_mechanics(config_v1::NormalizedDocument model, const MechanicalCheckpoint &checkpoint) {
    auto engine=Builder(model,checkpoint).build();
    try {
        // The legacy sweep uses exactly 1000 crank angles and the legacy
        // rod-placement calculation. Chamber checks occur after gas assembly.
        engine->calculateDisplacement();
    }
    catch (const std::runtime_error &error) {
        cfg::Diagnostic problem;
        problem.code="CFG_INITIALIZE";
        problem.stage="initialize";
        problem.pointer="/engine";
        problem.message=error.what();
        return problem;
    }
    if (checkpoint) checkpoint(MechanicalPhase::geometry,0);
    return MechanicalCandidate{std::move(model),std::move(engine)};
}

}
