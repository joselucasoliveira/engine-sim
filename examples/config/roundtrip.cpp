#include <engine_sim/config-json.hpp>

#include <clocale>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>

int main(int argc, char **argv) {
    namespace config = engine_sim::config_v1;
    const bool expand = argc == 4 && std::string_view(argv[1]) == "--expand";
    if (argc != 3 && !expand) {
        std::cerr << "Usage: engine-sim-config-roundtrip [--expand] INPUT.json OUTPUT.json\n";
        return 2;
    }
    std::setlocale(LC_ALL, "");
    const int first = expand ? 2 : 1;
    std::ifstream input(argv[first], std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    if (length < 0 || length > 16 * 1024 * 1024) {
        std::cerr << "Cannot open input or input exceeds the document size limit\n";
        return 1;
    }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(length))) {
        std::cerr << "Could not read the complete input file\n";
        return 1;
    }
    namespace json = engine_sim::config_json;
    const auto result = [&]() -> json::RewriteResult {
        if (!expand) return json::rewrite(bytes);
        auto model = json::read_model(bytes);
        if (auto *error = std::get_if<config::Diagnostic>(&model)) return std::move(*error);
        return config::write_json(std::get<config::NormalizedDocument>(model));
    }();
    if (const auto *error = std::get_if<config::Diagnostic>(&result)) {
        std::cerr << error->code << ' ' << error->pointer << ' ' << error->message << '\n';
        return 1;
    }
    const auto &output_bytes = std::get<std::string>(result);
    std::ofstream output(argv[first + 1], std::ios::binary);
    output.write(output_bytes.data(), static_cast<std::streamsize>(output_bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "Could not write the complete output file\n";
        return 1;
    }
}
