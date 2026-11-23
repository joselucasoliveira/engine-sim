#ifndef ENGINE_SIM_CONFIG_NORMALIZATION_HPP
#define ENGINE_SIM_CONFIG_NORMALIZATION_HPP

#include <engine_sim/config-v1.hpp>

namespace engine_sim::config_v1::detail {

// Defaults and binary64 representation are checked here. The graph validator
// consumes this intermediate before it can expose a NormalizedDocument.
class DefaultedValues final {
public:
    DefaultedValues(const DefaultedValues &) = default;
    DefaultedValues(DefaultedValues &&) noexcept = default;
    DefaultedValues &operator=(const DefaultedValues &) = default;
    DefaultedValues &operator=(DefaultedValues &&) noexcept = default;
    ~DefaultedValues() = default;

    const Document &values() const noexcept { return m_values; }

private:
    explicit DefaultedValues(Document input);
    Document m_values;

    friend std::variant<DefaultedValues, Diagnostic> normalize_values(Document input);
};

using DefaultsResult = std::variant<DefaultedValues, Diagnostic>;
DefaultsResult normalize_values(Document input);

}
#endif
