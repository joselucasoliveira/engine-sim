#include "json_session.hpp"
#include "camshaft.h"

#include <engine_sim/runtime.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
namespace json = engine_sim::json_runtime;
using Runtime = std::unique_ptr<es_runtime, decltype(&es_destroy)>;

void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

Runtime create() {
    es_error error{};
    es_config config{};
    require(es_config_init(ES_ABI_V2, sizeof(config), &config, &error) == ES_OK, error.message);
    config.seed = 403;
    config.output_hz = 44100;
    es_runtime *runtime = nullptr;
    require(es_create(&config, &runtime, &error) == ES_OK, error.message);
    return Runtime(runtime, es_destroy);
}

void load(es_runtime *runtime, const fs::path &input, const fs::path &resources) {
    es_error error{};
    require(es_load_json_file(runtime, input.u8string().c_str(), resources.u8string().c_str(),
                              nullptr, &error) == ES_OK, error.message);
}

es_telemetry snapshot(es_runtime *runtime) {
    es_telemetry state{};
    state.size = sizeof(state);
    state.abi = ES_ABI_V2;
    es_error error{};
    require(es_get_telemetry(runtime, &state, &error) == ES_OK, error.message);
    return state;
}

std::vector<float> render(es_runtime *runtime) {
    es_error error{};
    std::vector<float> output(24000);
    es_controls controls{sizeof(controls), ES_ABI_V2, 1, 1, .2, 0, 0};
    require(es_set_controls(runtime, &controls, &error) == ES_OK, error.message);
    uint32_t frames = 0;
    require(es_render(runtime, 8000, output.data(), 8000, &frames, &error) == ES_OK, error.message);
    require(frames == 8000, "Initial render prefix incomplete");
    controls.starter = 0;
    controls.demand = .5;
    require(es_set_controls(runtime, &controls, &error) == ES_OK, error.message);
    require(es_render(runtime, 16000, output.data() + 8000, 16000, &frames, &error) == ES_OK, error.message);
    require(frames == 16000, "Final render prefix incomplete");
    require(snapshot(runtime).measured_rpm > 0, "Engine did not start");
    return output;
}

bool same(const std::vector<float> &a, const std::vector<float> &b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

std::array<double, 2> desktop_profiles(const fs::path &path, const fs::path &root) {
    auto prepared = json::require_model(json::prepare_desktop(path, root, 403), path);
    const auto &engine = *prepared.session->definition.engine;
    const auto *a = engine.getHead(0)->getIntakeCamshaft()->getLobeProfile();
    const auto *b = engine.getHead(1)->getIntakeCamshaft()->getLobeProfile();
    require(a == b, "The authored profile lost its shared references");
    const auto *exhaust = engine.getHead(0)->getExhaustCamshaft()->getLobeProfile();
    require(exhaust != a, "Separate intake and exhaust profiles were merged");
    return {a->sampleTriangle(.6), exhaust->sampleTriangle(.6)};
}

void verify(const fs::path &root, const fs::path &before, const fs::path &edited, const fs::path &explicit_model,
            const fs::path &invalid) {
    auto runtime = create();
    load(runtime.get(), before, root);
    const auto original_samples = render(runtime.get());

    load(runtime.get(), edited, root);
    auto initial = snapshot(runtime.get());
    require(initial.frames == 0 && initial.ticks == 0 && initial.native_frames == 0, "Reload pre-rendered a frame");
    const auto edited_samples = render(runtime.get());
    require(!same(original_samples, edited_samples), "The duration edit did not affect the audio");

    auto control = create();
    load(control.get(), explicit_model, root);
    require(same(edited_samples, render(control.get())), "Edited v2 PCM differs from the independent explicit v1");
    const auto expected = snapshot(control.get());
    auto actual = snapshot(runtime.get());
    require(std::memcmp(&actual, &expected, sizeof(actual)) == 0, "Edited v2 telemetry differs from v1");

    es_error error{};
    es_diagnostic detail{};
    detail.size = sizeof(detail);
    detail.abi = ES_ABI_V2;
    require(es_load_json_file(runtime.get(), invalid.u8string().c_str(), root.u8string().c_str(),
                              &detail, &error) == ES_CONFIG, "Invalid reload did not fail at the SDK boundary");
    require(std::string_view(detail.field) == "gamma", "Invalid reload did not identify the authored field");
    actual = snapshot(runtime.get());
    require(std::memcmp(&actual, &expected, sizeof(actual)) == 0, "Invalid reload changed the live engine");
    require(es_reset(runtime.get(), &error) == ES_OK, error.message);
    require(same(edited_samples, render(runtime.get())), "Reset lost the edited authoring recipe");

    const auto desktop_before = desktop_profiles(before, root);
    const auto desktop_edited = desktop_profiles(edited, root);
    const auto desktop_explicit = desktop_profiles(explicit_model, root);
    require(desktop_edited == desktop_explicit, "Desktop preparation disagrees with the explicit profile");
    require(desktop_before[0] != desktop_edited[0] && desktop_before[1] == desktop_edited[1],
            "Desktop preparation did not isolate the duration edit to the intake");
    std::cout << "GM LS reloaded at frame zero, rendered the explicit-v1 PCM and retained the edited recipe on reset\n";
}
}

int main(int argc, char **argv) {
    if (argc != 6) return 2;
    try {
        verify(fs::u8path(argv[1]), fs::u8path(argv[2]), fs::u8path(argv[3]), fs::u8path(argv[4]), fs::u8path(argv[5]));
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
