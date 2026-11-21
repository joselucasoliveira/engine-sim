#include <SDL3/SDL_main.h>
#include "desktop_run.h"

int main(int argc, char **argv) {
    return desktop::runApplication(engine_sim::processArguments(argc,argv), desktop::makeSdlHost);
}
