#ifndef ATG_ENGINE_SIM_ENGINE_SESSION_H
#define ATG_ENGINE_SIM_ENGINE_SESSION_H

#include "engine.h"
#include "simulator.h"
#include "vehicle.h"
#include "transmission.h"
#include "application_settings.h"
#include "impulse_response.h"
#include "units.h"

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct EngineLoadDiagnostic {
    std::string stage, path, message;
    std::string format() const { return stage + ": " + path + ": " + message; }
};

class EngineLoadError : public std::runtime_error {
public:
    explicit EngineLoadError(const EngineLoadDiagnostic &diagnostic)
        : std::runtime_error(diagnostic.format()), diagnostic(diagnostic) {}
    EngineLoadDiagnostic diagnostic;
};

// One execution produces one transferable definition. Shared curves and IR
// metadata live with the engine, while the other two model owners are independent.
struct EngineDefinition {
    std::unique_ptr<Engine> engine;
    std::unique_ptr<Vehicle> vehicle;
    std::unique_ptr<Transmission> transmission;
    Simulator::Parameters simulatorParameters;
    ApplicationSettings applicationSettings;
    std::vector<Function *> functions;
    std::string sourcePath;
};

class EngineSession {
public:
    struct SimulatorRelease {
        void operator()(Simulator *simulator) const {
            if (simulator != nullptr) { simulator->releaseSimulation(); delete simulator; }
        }
    };
    using ImpulseReader = std::function<std::vector<int16_t>(const ImpulseResponse &)>;

    explicit EngineSession(EngineDefinition value) : definition(std::move(value)) {}

    static std::unique_ptr<EngineSession> prepare(EngineDefinition definition, const ImpulseReader &reader) {
        const auto path = definition.sourcePath;
        if (definition.engine == nullptr) throw EngineLoadError({"graph", path, "No engine was selected"});
        auto candidate = std::make_unique<EngineSession>(std::move(definition));
        try {
            auto &model = candidate->definition;
            if (model.vehicle == nullptr) {
                Vehicle::Parameters p;
                p.mass = units::mass(1597, units::kg);
                p.diffRatio = 3.42;
                p.tireRadius = units::distance(10, units::inch);
                p.dragCoefficient = 0.25;
                p.crossSectionArea = units::distance(6.0, units::foot) * units::distance(6.0, units::foot);
                p.rollingResistance = 2000;
                model.vehicle = std::make_unique<Vehicle>();
                model.vehicle->initialize(p);
            }
            if (model.transmission == nullptr) {
                const double ratios[] = {2.97, 2.07, 1.43, 1, 0.84, 0.56};
                Transmission::Parameters p{6, ratios, units::torque(1000, units::ft_lb)};
                model.transmission = std::make_unique<Transmission>();
                model.transmission->initialize(p);
            }
            auto *engine = model.engine.get();
            if (engine->getCylinderCount() <= 0 || engine->getCrankshaftCount() <= 0
                || engine->getExhaustSystemCount() <= 0 || engine->getIntakeCount() <= 0) {
                throw EngineLoadError({"graph", path, "Engine topology is incomplete"});
            }
            engine->calculateDisplacement();
            candidate->simulator.reset(engine->createSimulator(model.vehicle.get(), model.transmission.get()));
            candidate->simulator->setSimulationFrequency(engine->getSimulationFrequency());
            auto &synth = candidate->simulator->synthesizer();
            auto audio = synth.getAudioParameters();
            audio.inputSampleNoise = static_cast<float>(engine->getInitialJitter());
            audio.airNoise = static_cast<float>(engine->getInitialNoise());
            audio.dF_F_mix = static_cast<float>(engine->getInitialHighFrequencyGain());
            synth.setAudioParameters(audio);
            for (int i = 0; i < engine->getExhaustSystemCount(); ++i) {
                const auto *response = engine->getExhaustSystem(i)->getImpulseResponse();
                if (response == nullptr) throw EngineLoadError({"resources", path, "Missing impulse response"});
                const auto samples = reader(*response);
                synth.initializeImpulseResponse(samples.data(), static_cast<unsigned>(samples.size()),
                    static_cast<float>(response->getVolume()), i);
            }
            return candidate;
        }
        catch (const EngineLoadError &) { throw; }
        catch (const std::exception &error) { throw EngineLoadError({"initialize", path, error.what()}); }
    }

    EngineDefinition definition;
    std::unique_ptr<Simulator, SimulatorRelease> simulator;
};

#endif
