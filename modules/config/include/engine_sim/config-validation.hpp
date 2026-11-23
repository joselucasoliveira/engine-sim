#ifndef ENGINE_SIM_CONFIG_VALIDATION_HPP
#define ENGINE_SIM_CONFIG_VALIDATION_HPP

#include <engine_sim/config-v1.hpp>

#include <memory>

namespace engine_sim::config_v1 {

struct ResolvedCylinder {
    CylinderIndex index;
    std::size_t piston_definition{};
    std::size_t rod_definition{};
    std::size_t intake{};
    std::size_t exhaust{};
    std::size_t ignition_wire{};
    std::size_t root_crank{}; // Index in ResolvedTopology::crankshafts
};

struct ResolvedTopology {
    // Definition indices are kept in the selected construction order.
    std::vector<std::size_t> crankshafts;
    std::vector<std::size_t> banks;
    std::vector<std::size_t> intakes;
    std::vector<std::size_t> exhausts;
    std::vector<ResolvedCylinder> cylinders;
    // Indexed by the wire definition table, with members in cylinder order.
    std::vector<std::vector<std::size_t>> wire_cylinders;
};

class NormalizedDocument final {
public:
    NormalizedDocument(const NormalizedDocument &) = default;
    NormalizedDocument(NormalizedDocument &&) noexcept = default;
    NormalizedDocument &operator=(const NormalizedDocument &) = default;
    NormalizedDocument &operator=(NormalizedDocument &&) noexcept = default;
    ~NormalizedDocument() = default;

    // Views borrow this immutable value. A moved-from value supports assignment and destruction.
    const Document &values() const noexcept;
    const ResolvedTopology &topology() const noexcept;

private:
    struct State;
    explicit NormalizedDocument(std::shared_ptr<const State> state) noexcept;
    std::shared_ptr<const State> m_state;

    friend std::variant<NormalizedDocument, Diagnostic> normalize(Document input);
};

using ModelResult = std::variant<NormalizedDocument, Diagnostic>;
ModelResult normalize(Document input);

}
#endif
