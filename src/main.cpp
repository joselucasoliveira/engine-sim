#include "../include/engine_sim_application.h"
#include "desktop_run.h"

#include <iostream>
#include <windows.h>

int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nCmdShow)
{
    (void)nCmdShow;
    (void)lpCmdLine;
    (void)hPrevInstance;

    try {
        return desktop::runApplication(engine_sim::processArguments(0,nullptr),
            [&](const desktop::RunOptions &options) { return desktop::makeWindowsHost((void *)&hInstance,options.virtualAudio); });
    }
    catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
