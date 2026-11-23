#include "canonical.hpp"
#include "scenario.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

namespace {

FILE *open_output(const std::filesystem::path &path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

int render(const std::filesystem::path &asset_root, const std::string &rate, const std::string &mode,
           const std::filesystem::path &model, const std::filesystem::path &pcm) {
    es_error error{};
    es_config config{};
    if (!example_config(rate.c_str(), mode.c_str(), &config, &error)) {
        throw std::runtime_error("Choose 44100 or 48000 and free or driven");
    }
    es_runtime *handle = nullptr;
    if (es_create(&config, &handle, &error) != ES_OK) throw std::runtime_error(error.message);
    std::unique_ptr<es_runtime, decltype(&es_destroy)> runtime(handle, es_destroy);
    {
        const std::string bytes = engine_sim::example::canonical_model(std::cerr);
        std::ofstream json(model, std::ios::binary);
        json.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        json.close();
        if (!json) throw std::runtime_error("Could not write the canonical model");
        es_diagnostic detail{};
        detail.size = sizeof detail;
        detail.abi = ES_ABI_V2;
        const auto root = std::filesystem::absolute(asset_root).u8string();
        const auto *data = reinterpret_cast<const std::uint8_t *>(bytes.data());
        if (es_load_json_bytes(runtime.get(), data, bytes.size(), root.c_str(), &detail, &error) != ES_OK) {
            throw std::runtime_error(std::string(detail.code) + " " + detail.pointer + " " + error.message);
        }
    }
    // The runtime has pinned its own bytes and response. Rendering and reset
    // do not borrow the serialization buffer, which has left scope.
    const auto close_file = [](FILE *file) { std::fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> output(open_output(pcm), close_file);
    if (!output) throw std::runtime_error("Could not open the PCM output");
    if (!example_render(runtime.get(), &config, output.get(), &error)) throw std::runtime_error(error.message);
    if (std::fclose(output.release()) != 0) throw std::runtime_error("Could not close the PCM output");
    return 0;
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv)
#else
int main(int argc, char **argv)
#endif
{
    if (argc != 6) {
        std::cerr << "Usage: engine-sim-programmatic ASSET_ROOT RATE MODE OUTPUT.json OUTPUT.f32\n";
        return 2;
    }
    try {
        return render(argv[1], std::filesystem::path(argv[2]).u8string(),
                      std::filesystem::path(argv[3]).u8string(), argv[4], argv[5]);
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
