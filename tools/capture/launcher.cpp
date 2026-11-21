#include "capture_config.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

int captureWorker(int argc, char **argv);

#ifdef _WIN32
std::string quoteArgument(const std::string &argument) {
    std::string result = "\"";
    std::size_t slashes = 0;
    for (char c : argument) {
        if (c == '\\') {
            ++slashes;
            continue;
        }
        result.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, '\\');
    result += '"';
    return result;
}
#endif

int main(int argc, char **argv) {
    if (argc > 1 && std::strcmp(argv[1], "--worker") == 0) return captureWorker(argc, argv);
    const std::string launcher = std::filesystem::absolute(argv[0]).string();
    std::vector<std::string> args{
        capturePython, "-B", captureScript, launcher, captureMetadata, captureSource
    };
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#ifdef _WIN32
    std::vector<const char *> processArgs;
    for (auto &arg : args) {
        arg = quoteArgument(arg);
        processArgs.push_back(arg.c_str());
    }
    processArgs.push_back(nullptr);
    const auto result = _spawnv(_P_WAIT, capturePython, processArgs.data());
    if (result >= 0) return static_cast<int>(result);
#else
    std::vector<char *> processArgs;
    for (auto &arg : args) processArgs.push_back(arg.data());
    processArgs.push_back(nullptr);
    execv(capturePython, processArgs.data());
#endif
    std::cerr << "cannot start the configured Python interpreter: " << capturePython << '\n';
    return 2;
}
