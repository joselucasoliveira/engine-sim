#include "compiler.h"
#include "resource_paths.h"
#include "wave_reader.h"

#include <algorithm>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using es_script::Compiler;
using es_script::PreparationContext;
using es_script::ScriptInput;

void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    check(static_cast<bool>(stream), "cannot read fixture");
    const auto size = fs::file_size(path);
    std::string bytes(static_cast<std::size_t>(size), '\0');
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    check(static_cast<bool>(stream), "cannot read complete fixture");
    return bytes;
}

void chooseDecimalComma() {
    for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "de-DE", "German_Germany.1252"}) {
        if (std::setlocale(LC_NUMERIC, name) != nullptr && std::string(std::localeconv()->decimal_point) == ",") return;
    }
    throw std::runtime_error("test requires an installed decimal-comma C locale");
}

void valid(const PreparationContext &context, const fs::path &chosen, const fs::path &impulse) {
    chooseDecimalComma();
    const std::string numericLocale = std::setlocale(LC_NUMERIC, nullptr);
    const auto cwd = fs::current_path();
    std::vector<ScriptInput> inputs;
    const auto definition = Compiler::prepare(context, inputs);
    check(fs::current_path() == cwd, "preparation changed host cwd");
    check(std::setlocale(LC_NUMERIC, nullptr) == numericLocale, "preparation changed host numeric locale");
    check(definition.engine && definition.engine->getCylinderCount() == 2, "unable to use transferred model");
    check(std::abs(definition.engine->getCylinderBank(0)->getBore() - 0.083125) < 1e-12,
        "decimal literal was changed by host locale");
    const auto selected = fs::canonical(chosen);
    const auto entry = fs::canonical(context.entry);
    check(std::count_if(inputs.begin(), inputs.end(), [&](const ScriptInput &input) {
        return input.resolvedPath == selected;
    }) == 1, "chosen import was not recorded exactly once");
    check(!inputs.empty() && inputs.front().resolvedPath == entry, "entry is missing from inventory");
    for (const auto &input : inputs) {
        check(input.bytes == read(input.resolvedPath), "script inventory bytes differ from fixture");
    }
    const auto *ir = definition.engine->getExhaustSystem(0)->getImpulseResponse();
    check(ir && fs::equivalent(fs::u8path(ir->getFilename()), impulse), "IR did not resolve beside its declaring script");
    check(engine_sim::readImpulseWave(fs::u8path(ir->getFilename())) == std::vector<std::int16_t>{32767, -17},
        "wrong Unicode impulse bytes");
}

void rejected(const PreparationContext &context, const std::string &stage, const std::string &reason,
        const fs::path &recovery) {
    const auto cwd = fs::current_path();
    std::vector<ScriptInput> inputs{{context.entry, "previous committed bytes"}};
    for (int attempt = 0; attempt < 12; ++attempt) {
        bool failed = false;
        try { (void)Compiler::prepare(context, inputs); }
        catch (const Compiler::Error &error) {
            failed = true;
            if (error.diagnostic.stage != stage || error.diagnostic.message.find(reason) == std::string::npos) throw;
        }
        check(failed, "adverse script was accepted");
        check(inputs.size() == 1 && inputs[0].bytes == "previous committed bytes", "failure replaced inventory");
        check(fs::current_path() == cwd, "rejected preparation changed host cwd");
    }
    Compiler compiler;
    compiler.initialize({context.libraryRoot.u8string(), context.assetRoot.u8string()});
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::ostringstream diagnostics;
        check(!compiler.compile(context.entry.u8string(), diagnostics), "bad script compiled on a reused compiler");
        diagnostics.str("");
        check(compiler.compile(recovery.u8string(), diagnostics), "compiler failed to recover after rejection");
        const auto definition = compiler.execute();
        check(definition.engine && definition.engine->getCylinderCount() == 2,
            "recovered model is incomplete");
    }
}

void changed(const PreparationContext &context) {
    std::vector<ScriptInput> inputs;
    bool checkedBeforeParse = false;
    try {
        Compiler::prepare(context, inputs, [&](const ScriptInput &input) {
            check(input.resolvedPath == fs::canonical(context.entry), "verifier got wrong input identity");
            check(input.bytes == read(context.entry), "verifier did not receive actual bytes");
            checkedBeforeParse = true;
            throw Compiler::Error({"resources", input.resolvedPath.u8string(), "input changed"});
        });
    }
    catch (const Compiler::Error &error) {
        check(error.diagnostic.stage == "resources" && error.diagnostic.message == "input changed",
            "verifier error was changed before propagation");
    }
    check(checkedBeforeParse && inputs.empty(), "verifier did not precede parsing");
}
}

int main(int argc, char **argv) {
    try {
        const auto args = engine_sim::processArguments(argc, argv);
        check(args.size() >= 5, "missing arguments");
        const PreparationContext context{fs::u8path(args[2]), fs::u8path(args[3]), fs::u8path(args[4])};
        if (args[1] == "valid" && args.size() == 7) valid(context, fs::u8path(args[5]), fs::u8path(args[6]));
        else if (args[1] == "rejected" && args.size() == 8) rejected(context, args[5], args[6], fs::u8path(args[7]));
        else if (args[1] == "changed" && args.size() == 5) changed(context);
        else throw std::runtime_error("unknown test scenario");
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
