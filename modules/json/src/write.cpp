#include <engine_sim/config-json.hpp>

#include "read.hpp"
#include "record_fields.hpp"

#include <json.hpp>

#include <utility>

namespace engine_sim::config_v1 {
namespace {

class Writer {
public:
    std::string document(const Document &config) {
        value(config);
        m_output += '\n';
        return std::move(m_output);
    }

private:
    void scalar(const nlohmann::json &value) { m_output += value.dump(); }
    void value(const std::string &text) { scalar(text); }
    void value(const char *text) { scalar(text); }
    void value(bool flag) { scalar(flag); }
    void value(std::int32_t number) { scalar(number); }

    template<class Tag>
    void value(const Ref<Tag> &ref) { scalar(ref.value); }

    template<class Unit>
    void value(const Measure<Unit> &number) {
        // The locked serializer uses its binary64 Grisu2 path for a double,
        // retaining subnormals, the sign of zero, and the real suffix.
        scalar(number.value);
    }

    void value(double number) { scalar(number); }

    template<class T>
    void value(const std::optional<T> &record) { value(record.value()); }

    template<class... T>
    void value(const std::variant<T...> &variant) {
        std::visit([&](const auto &part) { value(part); }, variant);
    }

    template<class T>
    void value(const std::vector<T> &array) {
        m_output += '[';
        bool first = true;
        for (const auto &element : array) {
            separate(first);
            value(element);
        }
        m_output += ']';
    }

    template<class T>
    void value(const T &record) {
        m_output += '{';
        bool first = true;
        json_detail::RecordFields<T>{}(record, [&](const char *key, const auto &member) {
            separate(first);
            value(key);
            m_output += ':';
            value(member);
        });
        m_output += '}';
    }

    void separate(bool &first) {
        if (!std::exchange(first, false)) m_output += ',';
    }

    std::string m_output;
};
}

ModelResult read_json(std::string_view bytes) { return json_detail::read_normalized(bytes); }

std::string write_json(const NormalizedDocument &config) { return Writer{}.document(config.values()); }

}
