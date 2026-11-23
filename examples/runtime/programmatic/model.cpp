#include "canonical.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>

static int emit_model(const std::filesystem::path &path) {
    try {
        const auto bytes = engine_sim::example::canonical_model(std::cerr);
        std::ofstream output(path, std::ios::binary);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output) throw std::runtime_error("Could not write the canonical model");
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (argc != 2) {
        std::cerr << "Usage: engine-sim-model output.json\n";
        return 2;
    }
    return emit_model(argv[1]);
}
