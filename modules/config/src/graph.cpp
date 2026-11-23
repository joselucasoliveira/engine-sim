#include "validation_support.hpp"

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace engine_sim::config_v1::detail {
namespace {
enum class Kind {
    engine, function, journal, crankshaft, piston, rod, fuel, throttle, bank,
    head, valvetrain, camshaft, intake, exhaust, impulse, wire, ignition
};

constexpr std::array<std::string_view, 17> tables{
    "", "functions", "journals", "crankshafts", "pistons", "rods", "fuels", "throttles",
    "banks", "heads", "valvetrains", "camshafts", "intakes", "exhausts", "impulses", "wires", "ignitions"
};

struct Entry {
    Kind kind;
    std::size_t index;
    bool reached = false;
};

constexpr auto missing = std::numeric_limits<std::size_t>::max();

Cursor definition(Kind kind, std::size_t index, std::string_view id) {
    if (kind == Kind::engine) return Cursor{"/engine"}.owner(id);
    return Cursor{"/components"}.field(tables[static_cast<std::size_t>(kind)]).item(index).owner(id);
}

void check_float(const Cursor &where, double value) {
    if (std::abs(value) > std::numeric_limits<float>::max())
        reject(where, "CFG_NUMBER", "graph", "Value exceeds the legacy float conversion range");
}

template<class T>
std::string_view identity(const T &record) { return record.id.value; }

template<class... T>
std::string_view identity(const std::variant<T...> &record) {
    return std::visit([](const auto &value) { return identity(value); }, record);
}

class Graph {
public:
    explicit Graph(const Document &document)
        : m_document(document), m_parts(document.components),
          m_journals(m_parts.journals.size()), m_rods(m_parts.rods.size(), missing),
          m_intakes(m_parts.intakes.size(), missing), m_exhausts(m_parts.exhausts.size(), missing),
          m_heads(m_parts.heads.size(), false) {
        m_topology.wire_cylinders.resize(m_parts.wires.size());
    }

    ResolvedTopology build() {
        add(Kind::engine, 0, m_document.engine.id.value);
        index(Kind::function, m_parts.functions);
        index(Kind::journal, m_parts.journals);
        index(Kind::crankshaft, m_parts.crankshafts);
        index(Kind::piston, m_parts.pistons);
        index(Kind::rod, m_parts.rods);
        index(Kind::fuel, m_parts.fuels);
        index(Kind::throttle, m_parts.throttles);
        index(Kind::bank, m_parts.banks);
        index(Kind::head, m_parts.heads);
        index(Kind::valvetrain, m_parts.valvetrains);
        index(Kind::camshaft, m_parts.camshafts);
        index(Kind::intake, m_parts.intakes);
        index(Kind::exhaust, m_parts.exhausts);
        index(Kind::impulse, m_parts.impulses);
        index(Kind::wire, m_parts.wires);
        index(Kind::ignition, m_parts.ignitions);

        const auto &engine = m_document.engine;
        use(Kind::engine, m_document.selected_engine, Cursor{"/selected_engine", "selected_engine"});
        const auto where = definition(Kind::engine, 0, engine.id.value);
        validate_engine(where);

        const auto fuel_index = use(Kind::fuel, engine.fuel, where.field("fuel"));
        const auto &fuel = m_parts.fuels[fuel_index];
        curve(fuel.turbulence_to_flame_speed_ratio,
              definition(Kind::fuel, fuel_index, fuel.id.value).field("turbulence_to_flame_speed_ratio"));
        const auto throttle_index = use(Kind::throttle, engine.throttle, where.field("throttle"));
        validate_throttle(throttle_index);
        cranks(where.field("crankshafts"));
        banks(where.field("banks"));
        const auto ignition_index = use(Kind::ignition, engine.ignition, where.field("ignition"));
        ignition(ignition_index);
        curve(engine.mean_piston_speed_to_turbulence, where.field("mean_piston_speed_to_turbulence"));

        bind_journals();
        resolve_cranks();
        for (const auto id : m_order) {
            const auto &entry = m_entries.at(id);
            if (!entry.reached)
                reject(definition(entry.kind, entry.index, id).field("id"),
                       "CFG_REF", "graph", "Definition is outside the selected engine");
        }
        return std::move(m_topology);
    }

private:
    void add(Kind kind, std::size_t index, std::string_view id) {
        const auto where = definition(kind, index, {}).field("id");
        check_id(where, id);
        if (!m_entries.emplace(id, Entry{kind, index}).second)
            reject(where.owner(id), "CFG_DUPLICATE_ID", "graph", "ID already exists in the document");
        m_order.push_back(id);
    }

    template<class Record>
    void index(Kind kind, const std::vector<Record> &records) {
        for (std::size_t i = 0; i < records.size(); ++i) add(kind, i, identity(records[i]));
    }

    template<class Tag>
    std::size_t use(Kind kind, const Ref<Tag> &reference, const Cursor &where) {
        check_id(where, reference.value);
        const auto item = m_entries.find(reference.value);
        if (item == m_entries.end() || item->second.kind != kind) {
            const auto name = tables[static_cast<std::size_t>(kind)];
            const std::string expected = name.empty() ? "engine" : std::string(name);
            reject(where, "CFG_REF", "graph", "Expected a definition in " + expected + " for this reference");
        }
        item->second.reached = true;
        return item->second.index;
    }

    void validate_engine(const Cursor &where) const {
        const auto &engine = m_document.engine;
        check_float(where.field("hf_gain"), engine.hf_gain.value);
        check_float(where.field("jitter"), engine.jitter.value);
        check_float(where.field("noise"), engine.noise.value);
        if (m_document.vehicle->diff_ratio.value == 0)
            reject(Cursor{"/vehicle/diff_ratio", "diff_ratio"}, "CFG_TOPOLOGY", "graph", "Differential ratio must be nonzero");
        const auto &gears = m_document.transmission->gears;
        for (std::size_t i = 0; i < gears.size(); ++i) {
            if (gears[i].value == 0)
                reject(Cursor{"/transmission/gears", "gears"}.item(i), "CFG_TOPOLOGY", "graph", "Gear ratio must be nonzero");
        }
    }

    void validate_throttle(std::size_t index) const {
        const auto *g = std::get_if<Governor>(&m_parts.throttles[index]);
        if (!g) return;
        const auto where = definition(Kind::throttle, index, g->id.value);
        if (g->max_speed.value < g->min_speed.value)
            reject(where.field("max_speed"), "CFG_TOPOLOGY", "graph", "Governor speed bounds are reversed");
        if (g->max_v.value < g->min_v.value)
            reject(where.field("max_v"), "CFG_TOPOLOGY", "graph", "Governor velocity bounds are reversed");
    }

    void curve(const Ref<FunctionTag> &ref, const Cursor &where) {
        const auto index = use(Kind::function, ref, where);
        const auto &value = m_parts.functions[index];
        if (value.samples.size() >= 2 && value.filter_radius.value <= 0)
            reject(definition(Kind::function, index, value.id.value).field("filter_radius"),
                   "CFG_CURVE", "graph", "An active multivalued curve requires a positive filter radius");
    }

    template<class Owner>
    void claim_journals(const std::vector<Ref<RodJournalTag>> &journals,
                        const Cursor &list, std::size_t owner) {
        for (std::size_t i = 0; i < journals.size(); ++i) {
            const auto use_at = list.item(i);
            const auto id = use(Kind::journal, journals[i], use_at);
            if (m_journals[id].has_value())
                reject(use_at, "CFG_TOPOLOGY", "graph", "Journal already has an owner");
            m_journals[id] = Owner{owner, i};
        }
    }

    void cranks(const Cursor &where) {
        std::vector<bool> selected(m_parts.crankshafts.size(), false);
        const auto &refs = m_document.engine.crankshafts;
        for (std::size_t i = 0; i < refs.size(); ++i) {
            const auto index = use(Kind::crankshaft, refs[i], where.item(i));
            if (selected[index])
                reject(where.item(i), "CFG_TOPOLOGY", "graph", "Crankshaft selected twice");
            selected[index] = true;
            m_topology.crankshafts.push_back(index);
            const auto &crank = m_parts.crankshafts[index];
            const auto location = definition(Kind::crankshaft, index, crank.id.value);
            const double mass = crank.mass.value + crank.flywheel_mass.value;
            if (!std::isfinite(mass) || mass <= 0.0)
                reject(location.field("mass"), "CFG_NUMBER", "graph", "Combined crankshaft mass must be finite and positive");
            claim_journals<CrankJournalIndex>(crank.journals, location.field("journals"), i);
        }
    }

    void banks(const Cursor &where) {
        const auto &refs = m_document.engine.banks;
        std::vector<bool> selected(m_parts.banks.size(), false);
        for (std::size_t bank = 0; bank < refs.size(); ++bank) {
            const auto index = use(Kind::bank, refs[bank], where.item(bank));
            if (selected[index])
                reject(where.item(bank), "CFG_TOPOLOGY", "graph", "Cylinder bank selected twice");
            selected[index] = true;
            m_topology.banks.push_back(index);
            const auto &value = m_parts.banks[index];
            const auto at = definition(Kind::bank, index, value.id.value);
            const auto cylinders = at.field("cylinders");
            if (value.cylinders.size() > cylinder_limit - m_topology.cylinders.size())
                reject(cylinders, "CFG_LIMIT", "normalize", "Too many cylinders in the selected engine");
            for (std::size_t local = 0; local < value.cylinders.size(); ++local)
                cylinder(bank, local, value.cylinders[local], cylinders.item(local));
            head(value.head, value.cylinders.size(), at.field("head"));
        }
    }

    void cylinder(std::size_t bank, std::size_t local, const Cylinder &value, const Cursor &where) {
        const auto global = m_topology.cylinders.size();
        ResolvedCylinder result;
        result.index = CylinderIndex{bank, local, global, {}};
        result.piston_definition = use(Kind::piston, value.piston, where.field("piston"));
        const auto rod = use(Kind::rod, value.connecting_rod, where.field("connecting_rod"));
        if (m_rods[rod] != missing)
            reject(where.field("connecting_rod"), "CFG_TOPOLOGY", "graph", "Connecting rod belongs to another cylinder");
        m_rods[rod] = global;
        result.rod_definition = rod;
        const auto &rod_definition = m_parts.rods[rod];
        claim_journals<MasterRodJournalIndex>(rod_definition.journals,
            definition(Kind::rod, rod, rod_definition.id.value).field("journals"), global);
        m_used_journals.push_back(use(Kind::journal, value.rod_journal, where.field("rod_journal")));

        const auto intake = use(Kind::intake, value.intake, where.field("intake"));
        result.intake = first_use(intake, m_intakes, m_topology.intakes);
        const auto exhaust = use(Kind::exhaust, value.exhaust_system, where.field("exhaust_system"));
        if (m_exhausts[exhaust] == missing) {
            const auto &exhaust_definition = m_parts.exhausts[exhaust];
            const auto at = definition(Kind::exhaust, exhaust, exhaust_definition.id.value);
            const auto resource = use(Kind::impulse, exhaust_definition.impulse_response, at.field("impulse_response"));
            const auto &impulse = m_parts.impulses[resource];
            check_float(definition(Kind::impulse, resource, impulse.id.value).field("volume"), impulse.volume.value);
        }
        result.exhaust = first_use(exhaust, m_exhausts, m_topology.exhausts);
        result.ignition_wire = use(Kind::wire, value.ignition_wire, where.field("ignition_wire"));
        m_topology.wire_cylinders[result.ignition_wire].push_back(global);
        m_topology.cylinders.push_back(result);
    }

    static std::size_t first_use(std::size_t index, std::vector<std::size_t> &reverse,
                                 std::vector<std::size_t> &ordered) {
        if (reverse[index] == missing) {
            reverse[index] = ordered.size();
            ordered.push_back(index);
        }
        return reverse[index];
    }

    void head(const Ref<HeadTag> &id, std::size_t cylinders, const Cursor &where) {
        const auto index = use(Kind::head, id, where);
        if (m_heads[index]) reject(where, "CFG_TOPOLOGY", "graph", "Head is attached to another bank");
        m_heads[index] = true;
        const auto &value = m_parts.heads[index];
        const auto at = definition(Kind::head, index, value.id.value);
        curve(value.intake_port_flow, at.field("intake_port_flow"));
        curve(value.exhaust_port_flow, at.field("exhaust_port_flow"));
        const auto train = use(Kind::valvetrain, value.valvetrain, at.field("valvetrain"));
        std::visit([&](const auto &selected) {
            const auto in = definition(Kind::valvetrain, train, selected.id.value);
            camshaft(selected.intake_camshaft, cylinders, in.field("intake_camshaft"));
            camshaft(selected.exhaust_camshaft, cylinders, in.field("exhaust_camshaft"));
            if constexpr (std::is_same_v<std::decay_t<decltype(selected)>, Vtec>) {
                camshaft(selected.vtec_intake_camshaft, cylinders, in.field("vtec_intake_camshaft"));
                camshaft(selected.vtec_exhaust_camshaft, cylinders, in.field("vtec_exhaust_camshaft"));
            }
        }, m_parts.valvetrains[train]);
    }

    void camshaft(const Ref<CamshaftTag> &ref, std::size_t cylinders, const Cursor &where) {
        const auto index = use(Kind::camshaft, ref, where);
        const auto &value = m_parts.camshafts[index];
        const auto at = definition(Kind::camshaft, index, value.id.value);
        curve(value.lobe_profile, at.field("lobe_profile"));
        if (value.lobes.size() < cylinders)
            reject(at.field("lobes"), "CFG_TOPOLOGY", "graph", "Cam has fewer lobes than bank cylinders");
    }

    void ignition(std::size_t index) {
        const auto &value = m_parts.ignitions[index];
        const auto at = definition(Kind::ignition, index, value.id.value);
        curve(value.timing_curve, at.field("timing_curve"));
        for (std::size_t i = 0; i < value.posts.size(); ++i)
            use(Kind::wire, value.posts[i].wire, at.field("posts").item(i).field("wire"));
    }

    Cursor journal_location(const CylinderIndex &value) const {
        const auto bank = m_topology.banks[value.bank];
        return definition(Kind::bank, bank, m_parts.banks[bank].id.value)
            .field("cylinders").item(value.local).field("rod_journal");
    }

    void bind_journals() {
        for (std::size_t i = 0; i < m_topology.cylinders.size(); ++i) {
            auto &index = m_topology.cylinders[i].index;
            const auto &owner = m_journals[m_used_journals[i]];
            if (!owner)
                reject(journal_location(index), "CFG_TOPOLOGY", "graph", "Referenced journal has no selected owner");
            index.journal = *owner;
        }
        for (std::size_t i = 0; i < m_journals.size(); ++i) {
            if (!m_journals[i])
                reject(definition(Kind::journal, i, m_parts.journals[i].id.value).field("id"),
                       "CFG_TOPOLOGY", "graph", "Journal has no selected crankshaft or rod owner");
        }
    }

    void resolve_cranks() {
        auto &cylinders = m_topology.cylinders;
        enum class Mark { unseen, visiting, done };
        std::vector<Mark> seen(cylinders.size(), Mark::unseen);
        std::vector<std::size_t> chain;
        for (std::size_t i = 0; i < cylinders.size(); ++i) {
            if (seen[i] == Mark::done) continue;
            std::size_t next = i;
            std::size_t crank = missing;
            chain.clear();
            while (seen[next] != Mark::done) {
                auto &cylinder = cylinders[next];
                if (seen[next] == Mark::visiting)
                    reject(journal_location(cylinder.index), "CFG_TOPOLOGY", "graph", "Master rod ownership cycle");
                chain.push_back(next);
                seen[next] = Mark::visiting;
                if (const auto *direct = std::get_if<CrankJournalIndex>(&cylinder.index.journal)) {
                    crank = direct->crankshaft;
                    break;
                }
                next = std::get<MasterRodJournalIndex>(cylinder.index.journal).rod;
            }
            if (crank == missing) crank = cylinders[next].root_crank;
            for (const auto member : chain) {
                cylinders[member].root_crank = crank;
                seen[member] = Mark::done;
            }
        }
    }

    const Document &m_document;
    const Components &m_parts;
    std::unordered_map<std::string_view, Entry> m_entries;
    std::vector<std::string_view> m_order;
    std::vector<std::optional<JournalOwnerIndex>> m_journals;
    std::vector<std::size_t> m_rods;
    std::vector<std::size_t> m_intakes;
    std::vector<std::size_t> m_exhausts;
    std::vector<bool> m_heads;
    std::vector<std::size_t> m_used_journals;
    ResolvedTopology m_topology;
};
}

ResolvedTopology check_graph(const Document &input) { return Graph(input).build(); }

}
