#include "resource_paths.h"

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
std::wstring quoteArgument(const std::wstring &argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c; slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
#endif

int main(int argc, char **argv) {
    namespace fs = std::filesystem;
    try {
        auto arguments = engine_sim::processArguments(argc, argv);
        if (arguments.size() > 1 && arguments[1] == "--worker") {
            std::vector<char *> pointers;
            for (auto &arg : arguments) pointers.push_back(arg.data());
            return captureWorker(static_cast<int>(pointers.size()), pointers.data());
        }
        const auto executable = engine_sim::executablePath();
        const auto prefix = executable.parent_path().parent_path();
        auto helpers = engine_sim::ResourcePaths::defaultRoot(executable) / "tools/capture";
        if (!fs::is_regular_file(helpers / "capture_cli.py") && fs::is_regular_file(prefix / "CMakeCache.txt")) {
            helpers = prefix / "tools/capture";
        }
        for (const auto *name : {"capture_cli.py", "capture_metrics.py", "build.txt"}) {
            if (!fs::is_regular_file(helpers / name)) throw engine_sim::ResourceError(helpers / name, "capture helper is missing; build or install the capture target");
        }
        auto resources = engine_sim::ResourcePaths::defaultRoot(executable);
        if (arguments.size() > 1 && arguments[1] == "run") {
            const auto options = engine_sim::parseResourceOptions(arguments, true);
            const auto paths = engine_sim::ResourcePaths::resolve(executable, options);
            resources = paths.root;
            for (std::size_t i = 2; i < arguments.size(); ++i) {
                if (arguments[i] == "--resources") arguments[++i] = paths.root.u8string();
                else if (arguments[i] == "--model") arguments[++i] = paths.model.u8string();
            }
        }
        std::vector<std::string> common{"-B", (helpers / "capture_cli.py").u8string(), executable.u8string(),
            (helpers / "build.txt").u8string(), prefix.u8string(), resources.u8string()};
        common.insert(common.end(), arguments.begin() + 1, arguments.end());
#ifdef _WIN32
        for (const auto *python : {L"py", L"python3", L"python"}) {
            std::vector<std::wstring> processArgs{python};
            if (std::wstring(python) == L"py") processArgs.push_back(L"-3");
            for (const auto &arg : common) processArgs.push_back(fs::u8path(arg).native());
            std::vector<const wchar_t *> pointers;
            for (auto &arg : processArgs) { arg = quoteArgument(arg); pointers.push_back(arg.c_str()); }
            pointers.push_back(nullptr);
            const auto result = _wspawnvp(_P_WAIT, python, pointers.data());
            if (result >= 0) return static_cast<int>(result);
        }
#else
        std::vector<std::string> processArgs{"python3"};
        processArgs.insert(processArgs.end(), common.begin(), common.end());
        std::vector<char *> pointers;
        for (auto &arg : processArgs) pointers.push_back(arg.data());
        pointers.push_back(nullptr);
        execvp("python3", pointers.data());
#endif
        std::cerr << "Python 3.9 or newer must be available on PATH to run engine-sim-capture\n";
        return 2;
    }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 2; }
}
