#include <engine_sim/runtime.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    es_error error{};
    es_config config{};
    es_runtime *raw = nullptr;
    if (es_config_init(ES_ABI_V2, sizeof config, &config, &error) != ES_OK ||
        es_create(&config, &raw, &error) != ES_OK) return 1;
    std::unique_ptr<es_runtime, decltype(&es_destroy)> runtime(raw, es_destroy);
    if (es_load_json_file(raw, argv[1], argv[2], nullptr, &error) != ES_OK) return 1;
    std::array<float, 37> pcm{};
    std::uint32_t written = 0;
    if (es_render(raw, pcm.size(), pcm.data(), pcm.size(), &written, &error) != ES_OK || written != pcm.size()) return 1;

    const auto path = std::filesystem::u8path(argv[1]);
    std::ifstream input(path, std::ios::binary);
    const std::string original{std::istreambuf_iterator<char>(input), {}};
    if (original.empty()) return 1;
    input.close();
    {
        std::ofstream changed(path, std::ios::binary | std::ios::app);
        changed << '\n';
        changed.close();
        if (!changed) return 1;
    }
    es_diagnostic detail{};
    detail.size = sizeof detail;
    detail.abi = ES_ABI_V2;
    auto failed = es_reset_detailed(raw, &detail, &error);
    {
        std::ofstream restored(path, std::ios::binary | std::ios::trunc);
        restored.write(original.data(), static_cast<std::streamsize>(original.size()));
        restored.close();
        if (!restored) return 1;
    }
    es_telemetry telemetry{};
    telemetry.size = sizeof telemetry;
    telemetry.abi = ES_ABI_V2;
    if (failed != ES_RESOURCE || detail.stage != ES_DIAG_RESOURCE ||
        std::string_view(detail.code) != "CFG_RESOURCE_BYTES" || std::string_view(detail.path) != argv[1] ||
        es_get_telemetry(raw, &telemetry, &error) != ES_OK || telemetry.state != ES_READY ||
        telemetry.frames != written || es_reset_detailed(raw, &detail, &error) != ES_OK ||
        es_get_telemetry(raw, &telemetry, &error) != ES_OK || telemetry.frames || telemetry.native_frames || telemetry.ticks) {
        return 1;
    }
    std::cout << "CFG_RESOURCE_BYTES preserved the running graph and frame phase; the restored file reset successfully\n";
    return 0;
}
