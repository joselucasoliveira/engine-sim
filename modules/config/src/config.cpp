#include <engine_sim/config-validation.hpp>

#include "normalization.hpp"
#include "validation_support.hpp"

#include <utility>

namespace engine_sim::config_v1 {
struct NormalizedDocument::State {
    Document values;
    ResolvedTopology topology;
};

NormalizedDocument::NormalizedDocument(std::shared_ptr<const State> state) noexcept
    : m_state(std::move(state)) {}

const Document &NormalizedDocument::values() const noexcept { return m_state->values; }
const ResolvedTopology &NormalizedDocument::topology() const noexcept { return m_state->topology; }

ModelResult normalize(Document input) {
    try {
        detail::check_shape(input);
        auto resolved = detail::normalize_values(std::move(input));
        if (auto *error = std::get_if<Diagnostic>(&resolved)) return std::move(*error);
        auto values = std::get<detail::DefaultedValues>(std::move(resolved)).take();
        auto topology = detail::check_graph(values);
        return NormalizedDocument(std::make_shared<const NormalizedDocument::State>(
            NormalizedDocument::State{std::move(values), std::move(topology)}));
    }
    catch (detail::InvalidModel &invalid) {
        return std::move(invalid.detail);
    }
}

}
