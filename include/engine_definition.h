#ifndef ENGINE_SIM_ENGINE_DEFINITION_H
#define ENGINE_SIM_ENGINE_DEFINITION_H

#include "application_settings.h"
#include "simulator.h"

#include <memory>
#include <string>
#include <vector>

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

#endif
