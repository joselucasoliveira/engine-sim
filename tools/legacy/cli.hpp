#ifndef ENGINE_SIM_LEGACY_CLI_HPP
#define ENGINE_SIM_LEGACY_CLI_HPP

#include <json.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace engine_sim::legacy::cli {
namespace fs = std::filesystem;
using Json = nlohmann::json;

class Failure : public std::runtime_error {
public:
    Failure(int exit, std::string stage, std::string code, std::string message,
            fs::path path = {}, std::string pointer = {});
    int exit() const noexcept { return m_exit; }
    Json record() const;
private:
    int m_exit;
    std::string m_stage, m_code;
    fs::path m_path;
    std::string m_pointer;
};

struct Item {
    std::string key;
    fs::path entry, library, assets, resources;
    std::string selector = "entry";
    Json args = Json::object();
    std::optional<fs::path> output;
};

struct Plan {
    fs::path manifest;
    fs::path batch_path;
    bool force = false;
    std::vector<Item> items;
};

Plan arguments(const std::vector<std::string> &argv);
Json base_record(const Item &item, const Json &identity);
Json executable_identity();
std::string evaluate(const Item &item, const fs::path &manifest, Json &record);
std::string plan_identity(const Plan &plan);

std::string read_file(const fs::path &path, std::size_t limit, int exit);
void publish(const fs::path &target, std::string_view bytes, bool replace);
void protect(const fs::path &destination, const std::vector<fs::path> &inputs);
fs::path destination(std::string_view text);
int execute(const Plan &plan);

}
#endif
