#include "../include/engine_sim_application.h"

#include <iostream>

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)nCmdShow;
    (void)lpCmdLine;
    (void)hPrevInstance;

    EngineSimApplication application;
    int result = 0;
    try {
        const auto arguments = engine_sim::processArguments(0, nullptr);
        if (arguments.size() == 2 && arguments[1] == "--help") {
            std::cout << "Engine Simulator [--resources ROOT] [--script FILE]\n";
            return 0;
        }
        const auto resources = engine_sim::parseResourceOptions(arguments);
        application.initialize((void *)&hInstance, ysContextObject::DeviceAPI::DirectX11, resources);
        application.run();
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    application.destroy();

    return result;
}
