cmake_minimum_required(VERSION 3.25)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(EngineSimFleetExamples LANGUAGES C CXX)
    find_package(EngineSimFleet 1.0 CONFIG REQUIRED)
endif ()

add_executable(engine-sim-fleet-c-host host.c)
set_target_properties(engine-sim-fleet-c-host PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO LINKER_LANGUAGE CXX)
add_executable(engine-sim-fleet-cxx-host host.cpp)
set_target_properties(engine-sim-fleet-cxx-host PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

foreach(target IN ITEMS engine-sim-fleet-c-host engine-sim-fleet-cxx-host)
    target_link_libraries(${target} PRIVATE EngineSim::Fleet)
    if (COMMAND engine_sim_diagnose)
        engine_sim_diagnose(${target} TRUE)
    endif ()
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else ()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif ()
endforeach ()
