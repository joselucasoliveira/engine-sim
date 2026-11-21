cmake_minimum_required(VERSION 3.25)

file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" engine_sim_source)
file(REAL_PATH "${CMAKE_CURRENT_BINARY_DIR}" engine_sim_binary)
cmake_path(IS_PREFIX engine_sim_source "${engine_sim_binary}" NORMALIZE engine_sim_in_tree)
if (engine_sim_in_tree)
    message(FATAL_ERROR "Choose an absolute build directory outside the Engine Simulator checkout")
endif ()

if (DEFINED ENGINE_SIM_PRESET_ROOT AND NOT IS_ABSOLUTE "${ENGINE_SIM_PRESET_ROOT}")
    message(FATAL_ERROR "Set ES_OUT to an absolute directory outside the checkout")
endif ()

project(engine-sim LANGUAGES CXX)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif ()

set(engine_sim_windows FALSE)
if (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^[Xx]64$")
    set(engine_sim_windows TRUE)
endif ()
option(ENGINE_SIM_BUILD_DESKTOP "Build the Engine Simulator desktop app" ${engine_sim_windows})
option(PIRANHA_ENABLED "Enable the engine script interpreter" ${ENGINE_SIM_BUILD_DESKTOP})
option(DTV "Enable video output" OFF)
option(DISCORD_ENABLED "Enable Discord Rich Presence (Windows x64)" OFF)
option(BUILD_TESTING "Build and register the Engine Simulator tests" OFF)
option(ENGINE_SIM_BUILD_CAPTURE "Build the offline signal characterization tool" OFF)

if (ENGINE_SIM_BUILD_DESKTOP AND NOT engine_sim_windows)
    message(FATAL_ERROR "The current desktop backend requires Windows x64 with MSVC")
endif ()
if ((DTV OR DISCORD_ENABLED) AND NOT ENGINE_SIM_BUILD_DESKTOP)
    message(FATAL_ERROR "DTV and DISCORD_ENABLED require the Windows desktop app")
endif ()

if (ENGINE_SIM_BUILD_CAPTURE AND NOT PIRANHA_ENABLED)
    message(FATAL_ERROR "The capture tool requires PIRANHA_ENABLED")
endif ()

if (BUILD_TESTING OR PIRANHA_ENABLED OR ENGINE_SIM_BUILD_DESKTOP)
    set(ENGINE_SIM_DEPS_FILE "$ENV{ES_OUT}/deps.cmake" CACHE FILEPATH "Bootstrap CMake initializer")
    if (NOT EXISTS "${ENGINE_SIM_DEPS_FILE}")
        message(FATAL_ERROR "Missing ${ENGINE_SIM_DEPS_FILE}. Run tools/bootstrap.py for the selected profile and set ES_OUT or ENGINE_SIM_DEPS_FILE.")
    endif ()
    include("${ENGINE_SIM_DEPS_FILE}")
endif ()

if (BUILD_TESTING)
    include(CTest)
    if (NOT EXISTS "${ENGINE_SIM_GOOGLETEST_SOURCE}/CMakeLists.txt")
        message(FATAL_ERROR "GoogleTest is unavailable. Run tools/bootstrap.py with --tests.")
    endif ()
    set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
    if (MSVC)
        set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
    endif ()
    add_subdirectory("${ENGINE_SIM_GOOGLETEST_SOURCE}" googletest EXCLUDE_FROM_ALL)
endif ()

add_subdirectory(dependencies)

add_library(engine-sim STATIC
    # Source files
    src/audio_buffer.cpp
    src/camshaft.cpp
    src/crankshaft.cpp
    src/combustion_chamber.cpp
    src/connecting_rod.cpp
    src/convolution_filter.cpp
    src/cylinder_bank.cpp
    src/cylinder_head.cpp
    src/delay_filter.cpp
    src/derivative_filter.cpp
    src/direct_throttle_linkage.cpp
    src/dynamometer.cpp
    src/engine.cpp
    src/exhaust_system.cpp
    src/feedback_comb_filter.cpp
    src/filter.cpp
    src/fuel.cpp
    src/function.cpp
    src/gas_system.cpp
    src/gaussian_filter.cpp
    src/governor.cpp
    src/ignition_module.cpp
    src/impulse_response.cpp
    src/intake.cpp
    src/jitter_filter.cpp
    src/leveling_filter.cpp
    src/low_pass_filter.cpp
    src/part.cpp
    src/piston.cpp
    src/piston_engine_simulator.cpp
    src/simulator.cpp
    src/standard_valvetrain.cpp
    src/starter_motor.cpp
    src/synthesizer.cpp
    src/throttle.cpp
    src/transmission.cpp
    src/utilities.cpp
    src/valvetrain.cpp
    src/vehicle.cpp
    src/vehicle_drag_constraint.cpp
    src/vtec_valvetrain.cpp

    # Include files
    include/audio_buffer.h
    include/application_settings.h
    include/camshaft.h
    include/crankshaft.h
    include/combustion_chamber.h
    include/connecting_rod.h
    include/convolution_filter.h
    include/cylinder_bank.h
    include/cylinder_head.h
    include/delay_filter.h
    include/derivative_filter.h
    include/direct_throttle_linkage.h
    include/dynamometer.h
    include/engine.h
    include/exhaust_system.h
    include/feedback_comb_filter.h
    include/filter.h
    include/fuel.h
    include/function.h
    include/gas_system.h
    include/gaussian_filter.h
    include/governor.h
    include/ignition_module.h
    include/impulse_response.h
    include/intake.h
    include/jitter_filter.h
    include/leveling_filter.h
    include/low_pass_filter.h
    include/part.h
    include/piston.h
    include/piston_engine_simulator.h
    include/simulator.h
    include/standard_valvetrain.h
    include/starter_motor.h
    include/synthesizer.h
    include/throttle.h
    include/transmission.h
    include/units.h
    include/utilities.h
    include/valvetrain.h
    include/vehicle.h
    include/vehicle_drag_constraint.h
    include/vtec_valvetrain.h
)

find_package(Threads REQUIRED)
target_link_libraries(engine-sim PUBLIC simple-2d-constraint-solver PRIVATE Threads::Threads)
target_compile_features(engine-sim PUBLIC cxx_std_17)

target_include_directories(engine-sim PUBLIC include dependencies/submodules)

if (PIRANHA_ENABLED)
    add_library(engine-sim-script-interpreter STATIC
        # Source files
        scripting/src/channel_types.cpp
        scripting/src/compiler.cpp
        scripting/src/engine_context.cpp
        scripting/src/language_rules.cpp

        # Include files
        scripting/include/actions.h
        scripting/include/camshaft_node.h
        scripting/include/channel_types.h
        scripting/include/compiler.h
        scripting/include/connecting_rod_node.h
        scripting/include/crankshaft_node.h
        scripting/include/cylinder_bank_node.h
        scripting/include/cylinder_head_node.h
        scripting/include/engine_context.h
        scripting/include/engine_node.h
        scripting/include/engine_sim.h
        scripting/include/exhaust_system_node.h
        scripting/include/intake_node.h
        scripting/include/function_node.h
        scripting/include/ignition_module_node.h
        scripting/include/ignition_wire_node.h
        scripting/include/impulse_response_node.h
        scripting/include/intake_node.h
        scripting/include/language_rules.h
        scripting/include/node.h
        scripting/include/object_reference_node.h
        scripting/include/object_reference_node_output.h
        scripting/include/piranha.h
        scripting/include/piston_node.h
        scripting/include/rod_journal_node.h
        scripting/include/standard_valvetrain_node.h
        scripting/include/transmission_node.h
        scripting/include/valvetrain_node.h
        scripting/include/vtec_valvetrain_node.h
        scripting/include/vehicle_node.h
    )

    target_include_directories(engine-sim-script-interpreter PUBLIC scripting/include)

    target_link_libraries(engine-sim-script-interpreter PUBLIC engine-sim piranha)
    target_compile_definitions(engine-sim-script-interpreter PUBLIC ATG_ENGINE_SIM_PIRANHA_ENABLED)
endif ()

if (DISCORD_ENABLED)
    add_library(discord STATIC
        # Source files
        dependencies/discord/Discord.cpp

        # Include files
        dependencies/discord/Discord.h
        dependencies/discord/discord_register.h
        dependencies/discord/discord_rpc.h
    )
    set_property(TARGET discord PROPERTY FOLDER "discord")
endif ()

if (ENGINE_SIM_BUILD_DESKTOP)
    add_executable(engine-sim-app WIN32
        # Source files
        src/main.cpp
        src/engine_sim_application.cpp
        src/geometry_generator.cpp
        src/simulation_object.cpp
        src/piston_object.cpp
        src/connecting_rod_object.cpp
        src/ui_element.cpp
        src/ui_manager.cpp
        src/cylinder_pressure_gauge.cpp
        src/ui_math.cpp
        src/gauge.cpp
        src/crankshaft_object.cpp
        src/cylinder_bank_object.cpp
        src/cylinder_head_object.cpp
        src/ui_button.cpp
        src/ui_utilities.cpp
        src/combustion_chamber_object.cpp
        src/oscilloscope.cpp
        src/shaders.cpp
        src/engine_view.cpp
        src/right_gauge_cluster.cpp
        src/cylinder_temperature_gauge.cpp
        src/labeled_gauge.cpp
        src/throttle_display.cpp
        src/afr_cluster.cpp
        src/fuel_cluster.cpp
        src/oscilloscope_cluster.cpp
        src/performance_cluster.cpp
        src/firing_order_display.cpp
        src/load_simulation_cluster.cpp
        src/mixer_cluster.cpp
        src/info_cluster.cpp

        # Include files
        include/delta.h
        include/dtv.h
        include/engine_sim_application.h
        include/geometry_generator.h
        include/simulation_object.h
        include/piston_object.h
        include/connecting_rod_object.h
        include/ui_element.h
        include/ui_manager.h
        include/cylinder_pressure_gauge.h
        include/ui_math.h
        include/units.h
        include/crankshaft_object.h
        include/cylinder_bank_object.h
        include/cylinder_head_object.h
        include/ui_button.h
        include/ui_utilities.h
        include/combustion_chamber_object.h
        include/oscilloscope.h
        include/shaders.h
        include/engine_view.h
        include/right_gauge_cluster.h
        include/cylinder_temperature_gauge.h
        include/labeled_gauge.h
        include/throttle_display.h
        include/afr_cluster.h
        include/fuel_cluster.h
        include/oscilloscope_cluster.h
        include/performance_cluster.h
        include/firing_order_display.h
        include/load_simulation_cluster.h
        include/mixer_cluster.h
        include/info_cluster.h
    )

    target_link_libraries(engine-sim-app PRIVATE engine-sim delta-basic)

    set(windows_runtime
        "${ENGINE_SIM_D3DX10_DLL}" "${ENGINE_SIM_D3DX11_DLL}" "${ENGINE_SIM_D3DCOMPILER_DLL}"
        "${ENGINE_SIM_SDL2_DLL}" "${ENGINE_SIM_SDL2_IMAGE_DLL}")
    foreach(runtime IN LISTS windows_runtime)
        if (NOT EXISTS "${runtime}")
            message(FATAL_ERROR "Missing Windows runtime ${runtime}. Run the desktop bootstrap.")
        endif ()
    endforeach()
    add_custom_command(TARGET engine-sim-app POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${windows_runtime} "$<TARGET_FILE_DIR:engine-sim-app>"
        VERBATIM
    )

    if (DTV)
        target_link_libraries(engine-sim-app
            PRIVATE direct-to-video)
        target_compile_definitions(engine-sim-app PRIVATE ATG_ENGINE_SIM_VIDEO_CAPTURE)
    endif (DTV)

    if (PIRANHA_ENABLED)
        target_link_libraries(engine-sim-app
            PRIVATE engine-sim-script-interpreter)
    endif (PIRANHA_ENABLED)

    if (DISCORD_ENABLED)
        add_library(discord-rpc STATIC IMPORTED)
        set_property(TARGET discord-rpc PROPERTY IMPORTED_LOCATION
            "${PROJECT_SOURCE_DIR}/dependencies/discord/lib/discord-rpc.lib")
        target_link_libraries(engine-sim-app PRIVATE discord discord-rpc)
        target_compile_definitions(engine-sim-app PRIVATE ATG_ENGINE_SIM_DISCORD_ENABLED)
    endif ()

    target_include_directories(engine-sim-app PRIVATE dependencies/submodules)
endif ()

if (BUILD_TESTING)
    add_executable(engine-sim-test
        # Source files
        test/gas_system_tests.cpp
        test/function_test.cpp
        test/synthesizer_tests.cpp
    )

    target_link_libraries(engine-sim-test PRIVATE gtest_main engine-sim)

    include(GoogleTest)
    gtest_discover_tests(engine-sim-test DISCOVERY_TIMEOUT 30)
    add_executable(engine-sim-threaded-test test/threaded_synthesizer_tests.cpp)
    target_link_libraries(engine-sim-threaded-test PRIVATE gtest_main engine-sim)
    gtest_discover_tests(engine-sim-threaded-test
        DISCOVERY_TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        PROPERTIES LABELS "threaded" TIMEOUT 30)
    if (PIRANHA_ENABLED)
        add_subdirectory(test/script)
    endif ()
endif ()

if (ENGINE_SIM_BUILD_CAPTURE)
    add_subdirectory(tools/capture)
endif ()
