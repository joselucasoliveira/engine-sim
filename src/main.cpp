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
        application.initialize((void *)&hInstance, ysContextObject::DeviceAPI::DirectX11);
        application.run();
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    application.destroy();

    return result;
}
