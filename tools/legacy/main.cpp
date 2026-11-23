#include "cli.hpp"
#include "resource_paths.h"

#include <csignal>
#include <iostream>
#include <new>

int main(int argc,char **argv) {
    namespace cli=engine_sim::legacy::cli;
    try {
#ifdef SIGXFSZ
        std::signal(SIGXFSZ,SIG_IGN);
#endif
        const auto arguments=engine_sim::processArguments(argc,argv);
        if (arguments.size()==2 && arguments[1]=="--help") {
            std::cout <<
                "engine-sim-convert --entry PATH --library-root PATH --asset-root PATH --resource-root PATH\n"
                "  [--selector entry|main|factory:IDENT] [--arg NAME=JSON_SCALAR]...\n"
                "  (--output PATH|--validate-only) --manifest PATH [--force]\n"
                "engine-sim-convert --batch PLAN --manifest RESULT [--force]\n";
            return 0;
        }
        return cli::execute(cli::arguments(arguments));
    }
    catch (const cli::Failure &error) {
        std::cerr << error.record().dump() << '\n';
        return error.exit();
    }
    catch (const cli::Json::exception &error) {
        std::cerr << "Invalid converter arguments: " << error.what() << '\n';
        return 2;
    }
    catch (const std::bad_alloc &) {
        std::cerr << "Converter allocation failed\n";
        return 7;
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 7;
    }
}
