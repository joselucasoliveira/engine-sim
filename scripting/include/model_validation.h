#ifndef ATG_ENGINE_SIM_MODEL_VALIDATION_H
#define ATG_ENGINE_SIM_MODEL_VALIDATION_H

#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>

namespace es_script {
class ModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
inline void requireModel(bool condition, const std::string &message) {
    if (!condition) throw ModelError(message);
}
inline void requireFinite(double value, const std::string &name) {
    requireModel(std::isfinite(value), name + " must be finite");
}
inline void requirePositive(double value, const std::string &name) {
    requireModel(std::isfinite(value) && value > 0, name + " must be finite and positive");
}
inline void requireNonnegative(double value, const std::string &name) {
    requireModel(std::isfinite(value) && value >= 0, name + " must be finite and nonnegative");
}
inline int modelCount(std::size_t value, const std::string &name) {
    requireModel(value <= static_cast<std::size_t>(std::numeric_limits<int>::max()), name + " exceeds index range");
    return static_cast<int>(value);
}
}
#endif
