#ifndef ENGINE_SIM_PRIVATE_CONCRETE_VALIDATION_HPP
#define ENGINE_SIM_PRIVATE_CONCRETE_VALIDATION_HPP

#include "engine_session.h"

#include <cmath>
#include <filesystem>
#include <limits>

namespace engine_sim {
inline void requireConcreteModel(bool condition, const std::string &field, const std::filesystem::path &path) {
    if (!condition) throw EngineLoadError({"model", path.u8string(), field});
}

inline void validateConcreteGeometry(Engine &engine, const std::filesystem::path &entry) {
    const int cylinders = engine.getCylinderCount();
    requireConcreteModel(cylinders > 0 && cylinders <= std::numeric_limits<int>::max() / 8, "cylinder count overflow", entry);
    requireConcreteModel(engine.getCrankshaftCount() > 0 && engine.getCylinderBankCount() > 0
        && engine.getIntakeCount() > 0 && engine.getExhaustSystemCount() > 0, "incomplete topology", entry);
    for (int i = 0; i < cylinders; ++i) {
        const auto &piston = *engine.getPiston(i);
        const auto &rod = *piston.getRod();
        const auto &bank = *piston.getCylinderBank();
        requireConcreteModel(bank.getIndex() >= 0 && bank.getIndex() < engine.getCylinderBankCount(),
            "cylinder bank index", entry);
        const auto &head = *engine.getHead(bank.getIndex());
        requireConcreteModel(piston.getCylinderIndex() >= 0 && piston.getCylinderIndex() < bank.getCylinderCount(),
            "piston cylinder index", entry);
        const auto &exhaust = *head.getExhaustSystem(piston.getCylinderIndex());
        const double delaySamples = (head.getHeaderPrimaryLength(piston.getCylinderIndex()) +
            exhaust.getLength()) * 10000.0 / 343.0;
        requireConcreteModel(std::isfinite(delaySamples) && delaySamples > 0 &&
            delaySamples < std::numeric_limits<int>::max() - 32.0, "exhaust delay extent", entry);
        for (int sample = 0; sample < 1000; ++sample) {
            const double angle = 2 * constants::pi * sample / 1000;
            double x, y, orientation, travel;
            requireConcreteModel(placeRod(rod, bank, *rod.getCrankshaft(), angle, &x, &y, &orientation, &travel),
                "cylinder " + std::to_string(i) + " has unusable rod geometry", entry);
            const double volume = bank.boreSurfaceArea() *
                (bank.getDeckHeight() - travel - piston.getCompressionHeight()) +
                head.getCombustionChamberVolume() - piston.getDisplacement();
            requireConcreteModel(std::isfinite(volume) && volume > 0,
                "cylinder " + std::to_string(i) + " has nonpositive chamber volume", entry);
        }
    }
}

inline void validateConcreteModel(Engine &engine, double declaredFrequency, bool desktop,
                                  const std::filesystem::path &entry) {
    const auto effective = desktop && std::isfinite(declaredFrequency) ? std::trunc(declaredFrequency) : declaredFrequency;
    requireConcreteModel(std::isfinite(effective) && effective >= (desktop ? 400 : 4000) &&
                         effective <= (desktop ? 400000 : 96000) && std::floor(effective) == effective,
                         desktop ? "effective desktop simulation frequency must be in [400,400000]"
                                 : "simulation frequency must be an integer in [4000,96000]", entry);
    requireConcreteModel(std::isfinite(engine.getRedline()) && engine.getRedline() > 0, "invalid engine redline", entry);
    validateConcreteGeometry(engine, entry);
    for (const auto value : {engine.getInitialJitter(), engine.getInitialNoise(), engine.getInitialHighFrequencyGain()}) {
        requireConcreteModel(std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(),
                             "synthesizer parameter exceeds float range", entry);
    }
}
}
#endif
