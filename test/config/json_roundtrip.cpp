#include <engine_sim/config-json.hpp>

#include "record_fields.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace {
namespace cfg = engine_sim::config_v1;

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::string fixture_bytes(const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    require(length > 0 && length <= 16 * 1024 * 1024, "Cannot open fixture or wrong fixture extent");
    std::string bytes(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(length)))
        throw std::runtime_error("Cannot read complete fixture");
    return bytes;
}

cfg::NormalizedDocument verified(cfg::ModelResult result) {
    if (const auto *error = std::get_if<cfg::Diagnostic>(&result))
        throw std::runtime_error(error->code + " " + error->pointer + " " + error->message);
    return std::get<cfg::NormalizedDocument>(std::move(result));
}

class Witness {
    enum class Boundary { begin_record, end_record, begin_array, end_array };
    using Token = std::variant<Boundary, std::string, std::uint64_t, std::int32_t, bool>;

public:
    static bool equal(const cfg::Document &lhs, const cfg::Document &rhs) {
        Witness first, second;
        first.add(lhs);
        second.add(rhs);
        return first.m_tokens == second.m_tokens;
    }

private:
    void add(double number) {
        std::uint64_t bits;
        static_assert(sizeof number == sizeof bits);
        std::memcpy(&bits, &number, sizeof bits);
        m_tokens.emplace_back(bits);
    }

    void add(const std::string &value) { m_tokens.emplace_back(value); }
    void add(const char *value) { add(std::string(value)); }
    void add(bool value) { m_tokens.emplace_back(value); }
    void add(std::int32_t value) { m_tokens.emplace_back(value); }

    template<class Unit>
    void add(cfg::Measure<Unit> value) { add(value.value); }

    template<class Tag>
    void add(const cfg::Ref<Tag> &reference) { add(reference.value); }

    template<class T>
    void add(const std::optional<T> &record) { add(record.value()); }

    template<class... T>
    void add(const std::variant<T...> &variant) {
        std::visit([&](const auto &part) { add(part); }, variant);
    }

    template<class T>
    void add(const std::vector<T> &array) {
        m_tokens.emplace_back(Boundary::begin_array);
        for (const auto &element : array) add(element);
        m_tokens.emplace_back(Boundary::end_array);
    }

    template<class T>
    void add(const T &record) {
        m_tokens.emplace_back(Boundary::begin_record);
        cfg::json_detail::RecordFields<T>{}(record, [&](const char *key, const auto &field) {
            add(key);
            add(field);
        });
        m_tokens.emplace_back(Boundary::end_record);
    }

    std::vector<Token> m_tokens;
};

auto index_values(const cfg::ResolvedCylinder &part) {
    const auto journal = std::visit([](const auto &owner) {
        using Owner = std::decay_t<decltype(owner)>;
        if constexpr (std::is_same_v<Owner, cfg::CrankJournalIndex>)
            return std::pair{owner.crankshaft, owner.journal};
        else return std::pair{owner.rod, owner.journal};
    }, part.index.journal);
    return std::tuple{part.index.bank, part.index.local, part.index.global, part.index.journal.index(), journal,
                      part.piston_definition, part.rod_definition, part.intake, part.exhaust,
                      part.ignition_wire, part.root_crank};
}

std::string round_trip(const cfg::NormalizedDocument &first) {
    auto wire = cfg::write_json(first);
    auto second = verified(cfg::read_json(wire));
    require(Witness::equal(first.values(), second.values()), "Typed record or IEEE binary64 witness differs");
    const auto &left = first.topology(), &right = second.topology();
    require(left.crankshafts == right.crankshafts && left.banks == right.banks &&
            left.intakes == right.intakes && left.exhausts == right.exhausts &&
            left.wire_cylinders == right.wire_cylinders && left.cylinders.size() == right.cylinders.size(),
            "Reference identities or first encounter order differ");
    for (std::size_t i = 0; i < left.cylinders.size(); ++i)
        require(index_values(left.cylinders[i]) == index_values(right.cylinders[i]), "Cylinder indices differ");
    require(cfg::write_json(second) == wire, "Re-emission differs");
    return wire;
}

template<class T>
auto &duplicate(std::vector<T> &values, std::string_view new_id) {
    auto duplicate = values.at(0);
    duplicate.id.value = new_id;
    return values.emplace_back(std::move(duplicate));
}

void add_distinct_bank(cfg::Document &document) {
    auto &parts = document.components;
    auto &bank = duplicate(parts.banks, "z-bank");
    auto &cylinder = bank.cylinders.at(0);
    bank.head = duplicate(parts.heads, "z-head").id;
    cylinder.piston = duplicate(parts.pistons, "z-piston").id;
    cylinder.connecting_rod = duplicate(parts.rods, "z-rod").id;
    cylinder.rod_journal = duplicate(parts.journals, "z-journal").id;
    parts.crankshafts.at(0).journals.push_back(cylinder.rod_journal);
    cylinder.intake = duplicate(parts.intakes, "z-intake").id;
    cylinder.exhaust_system = duplicate(parts.exhausts, "z-exhaust").id;
    cylinder.ignition_wire = duplicate(parts.wires, "z-wire").id;
    auto &posts = parts.ignitions.at(0).posts;
    posts.insert(posts.begin(), {cylinder.ignition_wire, {0.5}});
    document.engine.banks.insert(document.engine.banks.begin(), bank.id);
}

void difficult_cases(const std::string &input) {
    auto original = verified(cfg::read_json(input));
    const auto simple = round_trip(original);
    require(simple.front() == '{' && simple.back() == '\n' &&
            simple.find('\n') == simple.size() - 1 &&
            simple.find("\"initialization\":{") != std::string::npos &&
            simple.find("\"vehicle\":{") != std::string::npos &&
            simple.find("\"transmission\":{") != std::string::npos &&
            simple.find("\"desktop\":{") != std::string::npos, "Canonical form lacks expanded defaults or exact LF");

    auto document = original.values();
    auto &engine = document.engine;
    using Real = std::numeric_limits<double>;
    engine.starter_speed.value = Real::lowest();
    engine.starter_torque.value = Real::max();
    engine.dyno_min_speed.value = std::nextafter(0.104719755, 1.0);
    engine.dyno_max_speed.value = std::nextafter(0.104719755, 0.0);
    engine.dyno_hold_step.value = 10.0 / 3.0;
    engine.jitter.value = -0.0;
    engine.noise.value = Real::denorm_min();
    engine.name = "Rota\xc3\xa7\xc3\xa3o \xf0\x9f\x94\xa7 \"\\/\b\f\n\r\t\x1f";
    document.components.camshafts.at(0).advance.value = Real::min();
    document.components.camshafts.at(0).lobes.push_back({0.125});
    document.transmission->gears = {{5.0 / 3.0}, {4.0 / 7.0}, {2.97}};
    document.desktop->color_orange = std::numeric_limits<std::int32_t>::min();
    document.desktop->color_red = std::numeric_limits<std::int32_t>::max();

    auto &parts = document.components;
    auto &samples = parts.functions.at(0).samples;
    samples.at(1).x = samples.at(0).x;
    require(samples.at(1).y.value != samples.at(0).y.value, "Fixture samples must carry distinct outputs");
    const auto pedal = std::get<cfg::DirectThrottle>(parts.throttles.at(0));
    cfg::Governor governor;
    governor.id = pedal.id;
    governor.min_speed = {62};
    governor.max_speed = {628};
    governor.min_v = {0};
    governor.max_v = {1};
    governor.k_s = {1};
    governor.k_d = {1};
    governor.gamma = pedal.gamma;
    parts.throttles.at(0) = governor;
    const auto original_train = std::get<cfg::Standard>(parts.valvetrains.at(0));
    cfg::Vtec vtec;
    vtec.id = original_train.id;
    vtec.intake_camshaft = original_train.intake_camshaft;
    vtec.exhaust_camshaft = original_train.exhaust_camshaft;
    vtec.vtec_intake_camshaft = original_train.intake_camshaft;
    vtec.vtec_exhaust_camshaft = original_train.exhaust_camshaft;
    parts.valvetrains.at(0) = vtec;
    add_distinct_bank(document);

    const auto complex = verified(cfg::normalize(document));
    require(complex.topology().banks == std::vector<std::size_t>{1, 0} &&
            complex.topology().intakes == std::vector<std::size_t>{1, 0} &&
            complex.topology().exhausts == std::vector<std::size_t>{1, 0},
            "Fixture did not exercise bank-major first encounters");
    require(complex.values().components.heads.at(0).intake_port_flow ==
            complex.values().components.heads.at(1).intake_port_flow,
            "Fixture lost a shared curve");
    const auto wire = round_trip(complex);
    require(wire.find("\"jitter\":-0.0") != std::string::npos &&
            wire.find("Rota\xc3\xa7\xc3\xa3o \xf0\x9f\x94\xa7 \\\"\\\\/\\b\\f\\n\\r\\t\\u001f") !=
                std::string::npos, "Signed zero or required canonical escapes differ");
    require(wire.find("\"banks\":[\"z-bank\",\"block\"]") != std::string::npos, "Bank order was changed");

    std::swap(document.engine.banks[0], document.engine.banks[1]);
    std::swap(samples[0], samples[1]);
    const auto reversed = verified(cfg::normalize(std::move(document)));
    require(reversed.topology().intakes != complex.topology().intakes &&
            !Witness::equal(complex.values(), reversed.values()) && round_trip(reversed) != wire,
            "Reordered first encounters or duplicate abscissas falsely compare equal");

    auto transient = wire;
    auto owned = verified(cfg::read_json(transient));
    transient.assign(transient.size(), '?');
    require(cfg::write_json(owned) == wire, "Parsed values borrowed the caller input buffer");
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        difficult_cases(fixture_bytes(argv[1]));
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
