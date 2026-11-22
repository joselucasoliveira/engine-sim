#ifndef ATG_ENGINE_SIM_ENGINE_NODE_H
#define ATG_ENGINE_SIM_ENGINE_NODE_H

#include "object_reference_node.h"

#include "crankshaft_node.h"
#include "cylinder_bank_node.h"
#include "ignition_module_node.h"
#include "engine_context.h"
#include "fuel_node.h"
#include "throttle_nodes.h"

#include "engine_sim.h"

#include <map>
#include <set>
#include <limits>
#include <algorithm>

namespace es_script {

    class EngineNode : public ObjectReferenceNode<EngineNode> {
    public:
        EngineNode() { /* void */ }
        virtual ~EngineNode() { /* void */ }

        void buildEngine(Engine *engine) {
            validate();
            int cylinderCount = 0;
            for (const CylinderBankNode *bank : m_cylinderBanks) {
                cylinderCount += bank->getCylinderCount();
            }

            // Shared systems follow their first cylinder-bank encounter. Heap
            // addresses cannot define channel routing across independent loads.
            std::vector<ExhaustSystemNode *> exhaustSystems;
            std::vector<IntakeNode *> intakes;
            for (const CylinderBankNode *bank : m_cylinderBanks) {
                const int n = bank->getCylinderCount();
                for (int i = 0; i < n; ++i) {
                    auto *exhaust = bank->getCylinder(i).exhaust;
                    auto *intake = bank->getCylinder(i).intake;
                    if (std::find(exhaustSystems.begin(), exhaustSystems.end(), exhaust) == exhaustSystems.end()) {
                        exhaustSystems.push_back(exhaust);
                    }
                    if (std::find(intakes.begin(), intakes.end(), intake) == intakes.end()) intakes.push_back(intake);
                }
            }

            EngineContext context;
            context.setEngine(engine);

            Engine::Parameters parameters = m_parameters;
            parameters.crankshaftCount = modelCount(m_crankshafts.size(), "crankshaft count");
            parameters.cylinderBanks = modelCount(m_cylinderBanks.size(), "cylinder bank count");
            parameters.cylinderCount = cylinderCount;
            parameters.exhaustSystemCount = modelCount(exhaustSystems.size(), "exhaust count");
            parameters.intakeCount = modelCount(intakes.size(), "intake count");
            parameters.throttle = m_throttle->generate();
            engine->initialize(parameters);

            {
                int i = 0;
                for (ExhaustSystemNode *exhaust : exhaustSystems) {
                    context.addExhaust(
                        exhaust, engine->getExhaustSystem(i++));
                }
            }

            {
                int i = 0;
                for (IntakeNode *intake : intakes) {
                    context.addIntake(
                        intake, engine->getIntake(i++));
                }
            }

            {
                int i = 0;
                for (const CylinderBankNode *bank : m_cylinderBanks) {
                    context.addHead(bank->getCylinderHead(), engine->getHead(i++));
                }
            }

            for (int i = 0; i < parameters.crankshaftCount; ++i) {
                m_crankshafts[i]->generate(engine->getCrankshaft(i), &context);
            }

            for (int i = 0; i < parameters.cylinderBanks; ++i) {
                m_cylinderBanks[i]->indexSlaveJournals(&context);
            }

            int cylinderIndex = 0;
            for (int i = 0; i < parameters.cylinderBanks; ++i) {
                m_cylinderBanks[i]->generate(
                    i,
                    cylinderIndex,
                    engine->getCylinderBank(i),
                    engine->getCrankshaft(0),
                    engine,
                    &context);
                cylinderIndex += m_cylinderBanks[i]->getCylinderCount();
            }

            for (int i = 0; i < parameters.cylinderBanks; ++i) {
                m_cylinderBanks[i]->connectRodAssemblies(&context);
            }

            // All master links now exist, including chains declared in other banks.
            for (int i = 0; i < parameters.cylinderCount; ++i) {
                auto *root = engine->getConnectingRod(i);
                while (root->getMasterRod() != nullptr) root = root->getMasterRod();
                engine->getConnectingRod(i)->setCrankshaft(root->getCrankshaft());
            }

            m_ignitionModule->generate(engine, &context);
            
            Function *meanPistonSpeedToTurbulence = engine->createResource<Function>();
            meanPistonSpeedToTurbulence->initialize(30, 1);
            for (int i = 0; i < 30; ++i) {
                const double s = (double)i;
                meanPistonSpeedToTurbulence->addSample(s, s * 0.5);
            }

            Fuel *fuel = engine->getFuel();
            m_fuel->generate(fuel, &context);

            CombustionChamber::Parameters ccParams;
            ccParams.CrankcasePressure = units::pressure(1.0, units::atm);
            ccParams.Fuel = fuel;
            ccParams.StartingPressure = units::pressure(1.0, units::atm);
            ccParams.StartingTemperature = units::celcius(25.0);
            ccParams.MeanPistonSpeedToTurbulence = meanPistonSpeedToTurbulence;

            for (int i = 0; i < engine->getCylinderCount(); ++i) {
                ccParams.Piston = engine->getPiston(i);
                ccParams.Head = engine->getHead(ccParams.Piston->getCylinderBank()->getIndex());
                engine->getChamber(i)->initialize(ccParams);
            }
        }

        void validate() const override {
            requireModel(m_ignitionModule != nullptr, "Missing ignition module");
            requireModel(m_throttle != nullptr && m_fuel != nullptr, "Missing throttle or fuel");
            requireModel(!m_crankshafts.empty() && !m_cylinderBanks.empty(), "Engine topology is empty");
            requirePositive(m_parameters.initialSimulationFrequency, "simulation frequency");
            requirePositive(m_parameters.redline, "engine redline");
            requireNonnegative(m_parameters.starterTorque, "starter torque");
            requireFinite(m_parameters.starterSpeed, "starter speed");
            m_throttle->validate();
            m_ignitionModule->validate();
            m_fuel->validate();
            std::set<CrankshaftNode *> shafts;
            std::set<const CylinderBankNode *> banks;
            std::set<RodJournalNode *> journals;
            std::map<ConnectingRodNode *, RodJournalNode *> routes;
            size_t count = 0;
            for (auto *shaft : m_crankshafts) {
                requireModel(shaft && shafts.insert(shaft).second, "Missing or duplicate crankshaft");
                shaft->validate();
                for (auto *journal : shaft->getJournals()) {
                    requireModel(journals.insert(journal).second && journal->getCrankshaft() == shaft
                        && journal->getRod() == nullptr, "Crank journal has inconsistent ownership");
                }
            }
            for (auto *bank : m_cylinderBanks) {
                requireModel(bank && banks.insert(bank).second, "Missing or duplicate cylinder bank");
                bank->validate();
                requireModel(bank->getCylinderHead()->getBank() == bank, "Cylinder head belongs to another bank");
                count += bank->getCylinderCount();
                requireModel(count <= static_cast<size_t>(std::numeric_limits<int>::max()), "Too many cylinders");
                for (int i = 0; i < bank->getCylinderCount(); ++i) {
                    const auto &cylinder = bank->getCylinder(i);
                    requireModel(routes.emplace(cylinder.rod, cylinder.rodJournal).second, "Rod is shared by cylinders");
                }
            }
            for (const auto &route : routes) {
                for (auto *journal : route.first->getJournals()) {
                    requireModel(journals.insert(journal).second && journal->getRod() == route.first
                        && journal->getCrankshaft() == nullptr, "Slave journal has inconsistent ownership");
                }
            }
            for (const auto &route : routes) {
                std::set<ConnectingRodNode *> visited;
                auto *rod = route.first;
                while (true) {
                    requireModel(visited.insert(rod).second, "Master rod graph contains a cycle");
                    const auto found = routes.find(rod);
                    requireModel(found != routes.end(), "Master rod is outside this engine");
                    auto *journal = found->second;
                    requireModel(journals.count(journal) != 0, "Rod journal is outside this engine");
                    if (journal->getRod() == nullptr) {
                        requireModel(shafts.count(journal->getCrankshaft()) != 0, "Journal crankshaft is outside this engine");
                        break;
                    }
                    rod = journal->getRod();
                }
            }
            for (const auto &post : m_ignitionModule->getPosts()) {
                for (const auto &connection : post.wire->getConnections()) {
                    requireModel(banks.count(connection.first) != 0 && connection.second >= 0
                        && connection.second < connection.first->getCylinderCount(), "Ignition wire targets an unknown cylinder");
                }
            }
        }

        void addCrankshaft(CrankshaftNode *crankshaft) {
            m_crankshafts.push_back(crankshaft);
        }

        void addCylinderBank(CylinderBankNode *bank) {
            m_cylinderBanks.push_back(bank);
        }

        int getIgnitionModuleCount() const {
            return m_ignitionModule == nullptr
                ? 0
                : 1;
        }

        void addIgnitionModule(IgnitionModuleNode *ignitionModule) {
            m_ignitionModule = ignitionModule;
        }

    protected:
        virtual void registerInputs() {
            addInput("name", &m_parameters.name);
            addInput("starter_torque", &m_parameters.starterTorque);
            addInput("starter_speed", &m_parameters.starterSpeed);
            addInput("dyno_min_speed", &m_parameters.dynoMinSpeed);
            addInput("dyno_max_speed", &m_parameters.dynoMaxSpeed);
            addInput("dyno_hold_step", &m_parameters.dynoHoldStep);
            addInput("redline", &m_parameters.redline);
            addInput("fuel", &m_fuel, InputTarget::Type::Object);
            addInput("throttle", &m_throttle, InputTarget::Type::Object);
            addInput("simulation_frequency", &m_parameters.initialSimulationFrequency);
            addInput("hf_gain", &m_parameters.initialHighFrequencyGain);
            addInput("jitter", &m_parameters.initialJitter);
            addInput("noise", &m_parameters.initialNoise);

            ObjectReferenceNode<EngineNode>::registerInputs();
        }

        virtual void _evaluate() {
            setOutput(this);

            // Read inputs
            readAllInputs();
        }

        ThrottleNode *m_throttle = nullptr;
        IgnitionModuleNode *m_ignitionModule = nullptr;
        FuelNode *m_fuel = nullptr;

        Engine::Parameters m_parameters;
        std::vector<CrankshaftNode *> m_crankshafts;
        std::vector<CylinderBankNode *> m_cylinderBanks;
    };

} /* namespace es_script */

#endif /* ATG_ENGINE_SIM_ENGINE_NODE_H */
