#include <engine_sim/config-json.hpp>

#include <clocale>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
    namespace config = engine_sim::config_v1;
    if (argc != 3) {
        std::cerr << "Usage: engine-sim-config-roundtrip INPUT.json OUTPUT.json\n";
        return 2;
    }
    std::setlocale(LC_ALL, "");
    std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
    const auto length = input.tellg();
    if (length < 0 || length > 16 * 1024 * 1024) {
        std::cerr << "Cannot open input or input exceeds the v1 size limit\n";
        return 1;
    }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(length))) {
        std::cerr << "Could not read the complete input file\n";
        return 1;
    }
    const auto result = config::read_json(bytes);
    if (const auto *error = std::get_if<config::Diagnostic>(&result)) {
        std::cerr << error->code << ' ' << error->pointer << ' ' << error->message << '\n';
        return 1;
    }
    const auto output_bytes = config::write_json(std::get<config::NormalizedDocument>(result));
    std::ofstream output(argv[2], std::ios::binary);
    output.write(output_bytes.data(), static_cast<std::streamsize>(output_bytes.size()));
    output.close();
    if (!output) {
        std::cerr << "Could not write the complete output file\n";
        return 1;
    }
}
