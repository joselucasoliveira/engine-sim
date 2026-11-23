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
option(ENGINE_SIM_BUILD_RUNTIME "Build the device-free C runtime" OFF)
option(ENGINE_SIM_RUNTIME_SHARED "Build the C runtime as a shared library" OFF)
option(ENGINE_SIM_INSTALL_RUNTIME_EXAMPLE "Install the lawful synthetic runtime example" OFF)
option(ENGINE_SIM_BUILD_RUNTIME_EXAMPLES "Build public C API offline examples" OFF)
option(ENGINE_SIM_BUILD_HOST_TESTS "Build the desktop host contract fixtures" ON)
option(ENGINE_SIM_BUILD_LEGACY_CONVERTER "Build the optional legacy migration adapter" OFF)
set(engine_sim_parser_default ${ENGINE_SIM_BUILD_DESKTOP})
if (ENGINE_SIM_BUILD_RUNTIME OR ENGINE_SIM_BUILD_LEGACY_CONVERTER)
    set(engine_sim_parser_default ON)
endif ()
option(PIRANHA_ENABLED "Enable the engine script interpreter" ${engine_sim_parser_default})
if (ENGINE_SIM_BUILD_LEGACY_CONVERTER AND NOT PIRANHA_ENABLED)
    message(FATAL_ERROR "The legacy converter requires PIRANHA_ENABLED")
endif ()
option(DTV "Enable video output" OFF)
option(DISCORD_ENABLED "Enable Discord Rich Presence (Windows x64)" OFF)
option(BUILD_TESTING "Build and register the Engine Simulator tests" OFF)
option(ENGINE_SIM_BUILD_CAPTURE "Build the offline signal characterization tool" OFF)
include(cmake/Diagnostics.cmake)

if (ENGINE_SIM_BUILD_DESKTOP AND NOT engine_sim_windows
        AND NOT CMAKE_SYSTEM_NAME MATCHES "^(Linux|Darwin)$")
    message(FATAL_ERROR "Desktop adapters require Windows x64 with MSVC, Linux, or macOS")
endif ()
if (ENGINE_SIM_BUILD_DESKTOP AND NOT PIRANHA_ENABLED)
    message(FATAL_ERROR "Desktop adapters require PIRANHA_ENABLED")
endif ()
if ((DTV OR DISCORD_ENABLED) AND (NOT ENGINE_SIM_BUILD_DESKTOP OR NOT engine_sim_windows))
    message(FATAL_ERROR "DTV and DISCORD_ENABLED require the Windows desktop app")
endif ()

if (ENGINE_SIM_BUILD_RUNTIME AND NOT PIRANHA_ENABLED)
    message(FATAL_ERROR "The runtime requires PIRANHA_ENABLED")
endif ()
if (ENGINE_SIM_BUILD_RUNTIME_EXAMPLES AND NOT ENGINE_SIM_BUILD_RUNTIME)
    message(FATAL_ERROR "Runtime examples require ENGINE_SIM_BUILD_RUNTIME")
endif ()
if (ENGINE_SIM_BUILD_RUNTIME AND ENGINE_SIM_RUNTIME_SHARED AND BUILD_TESTING)
    message(FATAL_ERROR "Use runtime-test for private static tests and runtime-shared for isolated public SDK consumers")
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
    include(GoogleTest)
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

add_subdirectory(modules/config)
if (BUILD_TESTING OR ENGINE_SIM_BUILD_RUNTIME OR ENGINE_SIM_BUILD_LEGACY_CONVERTER)
    add_subdirectory(modules/json)
endif ()
if (BUILD_TESTING)
    add_subdirectory(examples/config)
endif ()

add_subdirectory(dependencies)
add_subdirectory(modules/synthesis)
add_subdirectory(modules/physics)

add_library(engine-sim STATIC
    $<TARGET_OBJECTS:engine-sim-synthesis-objects>
    $<TARGET_OBJECTS:engine-sim-physics-objects>
    # Source files
    src/audio_buffer.cpp
    src/feedback_comb_filter.cpp
    src/sample_clock.cpp
    src/pull_engine.cpp
    src/wave_reader.cpp

    # Include files
    include/audio_buffer.h
    include/application_settings.h
    include/camshaft.h
    include/crankshaft.h
    include/combustion_chamber.h
    include/compiled_scene.h
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
    include/resource_paths.h
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
    include/wave_reader.h
)

find_package(Threads REQUIRED)
target_link_libraries(engine-sim PUBLIC simple-2d-constraint-solver PRIVATE Threads::Threads)
target_compile_features(engine-sim PUBLIC cxx_std_17)
if (BUILD_TESTING)
    add_subdirectory(modules/assembly)
    add_subdirectory(test/config)
endif ()
if (NOT ENGINE_SIM_BUILD_RUNTIME)
    target_sources(engine-sim PRIVATE src/compiled_scene.cpp src/resource_paths.cpp)
    if (WIN32)
        target_link_libraries(engine-sim PRIVATE shell32)
    endif ()
elseif (BUILD_TESTING OR ENGINE_SIM_BUILD_DESKTOP OR ENGINE_SIM_BUILD_CAPTURE)
    add_library(engine-sim-host-resources STATIC src/compiled_scene.cpp src/resource_paths.cpp)
    target_include_directories(engine-sim-host-resources PUBLIC include)
    target_compile_features(engine-sim-host-resources PUBLIC cxx_std_17)
    if (WIN32)
        target_link_libraries(engine-sim-host-resources PRIVATE shell32)
    endif ()
endif ()

target_include_directories(engine-sim PUBLIC include dependencies/submodules)

if (PIRANHA_ENABLED)
    add_library(engine-sim-script-interpreter STATIC
        # Source files
        scripting/src/channel_types.cpp
        scripting/src/compiler.cpp
        scripting/src/engine_context.cpp
        scripting/src/language_rules.cpp
        scripting/src/prepared_engine.cpp

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

if (ENGINE_SIM_BUILD_LEGACY_CONVERTER)
    add_subdirectory(tools/legacy)
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

if (PIRANHA_ENABLED AND (ENGINE_SIM_BUILD_RUNTIME OR ENGINE_SIM_BUILD_DESKTOP OR ENGINE_SIM_BUILD_CAPTURE OR (BUILD_TESTING AND ENGINE_SIM_BUILD_HOST_TESTS)))
    add_library(engine-sim-runtime-backend STATIC src/runtime/runtime.cpp)
    target_link_libraries(engine-sim-runtime-backend PRIVATE engine-sim-script-interpreter)
    target_compile_features(engine-sim-runtime-backend PRIVATE cxx_std_17)
    engine_sim_diagnose(engine-sim-runtime-backend TRUE)
endif ()

if ((BUILD_TESTING AND ENGINE_SIM_BUILD_HOST_TESTS) OR ENGINE_SIM_BUILD_DESKTOP)
    add_library(engine-sim-host STATIC src/desktop_math.cpp src/desktop_host.cpp src/desktop_audio_pacer.cpp)
    target_include_directories(engine-sim-host PUBLIC include)
    target_compile_features(engine-sim-host PUBLIC cxx_std_17)
    if (NOT MSVC)
        target_compile_options(engine-sim-host PRIVATE -ffp-contract=off)
    endif ()
endif ()

if (PIRANHA_ENABLED AND ((BUILD_TESTING AND ENGINE_SIM_BUILD_HOST_TESTS) OR ENGINE_SIM_BUILD_DESKTOP))
    add_library(engine-sim-desktop-runtime STATIC src/runtime/desktop_runtime.cpp src/desktop_engine_views.cpp)
    target_link_libraries(engine-sim-desktop-runtime PRIVATE engine-sim-runtime-backend engine-sim-script-interpreter)
    target_compile_features(engine-sim-desktop-runtime PRIVATE cxx_std_17)
    engine_sim_diagnose(engine-sim-desktop-runtime TRUE)
    if (NOT MSVC)
        target_compile_options(engine-sim-desktop-runtime PRIVATE -ffp-contract=off)
    endif ()
    add_library(engine-sim-frontend STATIC
        src/engine_sim_application.cpp
        src/desktop_run.cpp
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
    )
    target_link_libraries(engine-sim-frontend PUBLIC engine-sim-script-interpreter engine-sim-host
        PRIVATE engine-sim-desktop-runtime)
    if (TARGET engine-sim-host-resources)
        target_link_libraries(engine-sim-frontend PUBLIC engine-sim-host-resources)
    endif ()
    if (NOT MSVC)
        target_compile_options(engine-sim-frontend PRIVATE -ffp-contract=off)
    endif ()
endif ()

if (ENGINE_SIM_BUILD_DESKTOP AND engine_sim_windows)
    add_executable(engine-sim-app WIN32 src/main.cpp src/app.manifest src/windows_host.cpp)

    target_link_libraries(engine-sim-app PRIVATE engine-sim-frontend delta-basic)

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
        target_link_libraries(engine-sim-frontend PUBLIC direct-to-video)
        target_compile_definitions(engine-sim-frontend PUBLIC ATG_ENGINE_SIM_VIDEO_CAPTURE)
    endif (DTV)

    if (PIRANHA_ENABLED)
        target_link_libraries(engine-sim-app
            PRIVATE engine-sim-script-interpreter)
    endif (PIRANHA_ENABLED)

    if (DISCORD_ENABLED)
        add_library(discord-rpc STATIC IMPORTED)
        set_property(TARGET discord-rpc PROPERTY IMPORTED_LOCATION
            "${PROJECT_SOURCE_DIR}/dependencies/discord/lib/discord-rpc.lib")
        target_link_libraries(engine-sim-frontend PUBLIC discord discord-rpc)
        target_compile_definitions(engine-sim-frontend PUBLIC ATG_ENGINE_SIM_DISCORD_ENABLED)
    endif ()

    target_include_directories(engine-sim-app PRIVATE dependencies/submodules)
endif ()

if (ENGINE_SIM_BUILD_DESKTOP AND NOT WIN32)
    include(cmake/Sdl.cmake)
    add_executable(engine-sim-app src/unix_main.cpp src/sdl_host.cpp)
    target_link_libraries(engine-sim-app PRIVATE engine-sim-frontend engine-sim-sdl-window
        engine-sim-sdl-renderer engine-sim-sdl-audio SDL3::SDL3-static)
endif ()

if (BUILD_TESTING)
    if (TARGET engine-sim-host)
        add_executable(engine-sim-host-test test/host_contract_tests.cpp)
        target_link_libraries(engine-sim-host-test PRIVATE gtest_main engine-sim-host)
        gtest_discover_tests(engine-sim-host-test DISCOVERY_TIMEOUT 30
            PROPERTIES LABELS host-contract TIMEOUT 30)
    endif ()
    if (TARGET engine-sim-frontend)
        add_executable(engine-sim-desktop-run-test test/desktop_run_tests.cpp)
        target_link_libraries(engine-sim-desktop-run-test PRIVATE gtest_main engine-sim-frontend)
        file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/run-working")
        gtest_discover_tests(engine-sim-desktop-run-test DISCOVERY_TIMEOUT 30
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/run-working"
            PROPERTIES LABELS desktop-smoke-unit TIMEOUT 30)
        add_executable(engine-sim-host-application-test test/host_application_tests.cpp)
        target_link_libraries(engine-sim-host-application-test PRIVATE gtest_main engine-sim-frontend)
        if (WIN32)
            target_sources(engine-sim-host-application-test PRIVATE src/app.manifest)
        endif ()
        file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/host-working")
        gtest_discover_tests(engine-sim-host-application-test TEST_FILTER "HostApplication.*" DISCOVERY_TIMEOUT 30
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/host-working"
            PROPERTIES LABELS host-contract TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_RESOURCES=${PROJECT_SOURCE_DIR}")
        gtest_discover_tests(engine-sim-host-application-test TEST_FILTER "ViewsApplication.*" DISCOVERY_TIMEOUT 30
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/host-working"
            PROPERTIES LABELS r2-views TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_RESOURCES=${PROJECT_SOURCE_DIR}")
    endif ()
    add_executable(engine-sim-test
        # Source files
        test/gas_system_tests.cpp
        test/function_test.cpp
        test/synthesizer_tests.cpp
    )

    target_link_libraries(engine-sim-test PRIVATE gtest_main engine-sim)

    gtest_discover_tests(engine-sim-test DISCOVERY_TIMEOUT 30)
    add_executable(engine-sim-threaded-test test/threaded_synthesizer_tests.cpp)
    target_link_libraries(engine-sim-threaded-test PRIVATE gtest_main engine-sim)
    gtest_discover_tests(engine-sim-threaded-test
        DISCOVERY_TIMEOUT 30
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        PROPERTIES LABELS "threaded" TIMEOUT 30)
    add_executable(engine-sim-wave-test test/wave_reader_tests.cpp)
    target_link_libraries(engine-sim-wave-test PRIVATE gtest engine-sim)
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/wave-working")
    gtest_discover_tests(engine-sim-wave-test
        DISCOVERY_TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/wave-working"
        PROPERTIES LABELS impulse TIMEOUT 90 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
    add_executable(engine-sim-resource-test test/resource_tests.cpp)
    target_link_libraries(engine-sim-resource-test PRIVATE gtest_main engine-sim)
    if (TARGET engine-sim-host-resources)
        target_link_libraries(engine-sim-resource-test PRIVATE engine-sim-host-resources)
    endif ()
    file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/resources-working")
    gtest_discover_tests(engine-sim-resource-test
        DISCOVERY_TIMEOUT 30 WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/resources-working"
        PROPERTIES LABELS resources TIMEOUT 90 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
    if (PIRANHA_ENABLED)
        find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
        add_test(NAME ImpulseReference
            COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/test/impulse_reference.py"
                "$<TARGET_FILE:engine-sim-wave-test>" "${PROJECT_SOURCE_DIR}")
        set_tests_properties(ImpulseReference PROPERTIES LABELS impulse TIMEOUT 180
            WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/wave-working")
    endif ()
    if (PIRANHA_ENABLED)
        add_subdirectory(test/script)
        if (TARGET engine-sim-host)
            add_executable(engine-sim-pacing-test test/desktop_audio_pacing_tests.cpp)
            target_link_libraries(engine-sim-pacing-test PRIVATE gtest_main engine-sim-host engine-sim-script-interpreter engine-sim-desktop-runtime)
            engine_sim_diagnose(engine-sim-pacing-test TRUE)
            file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/pacing-working")
            gtest_discover_tests(engine-sim-pacing-test TEST_FILTER "DesktopPacing.*" DISCOVERY_TIMEOUT 30
                WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/pacing-working"
                PROPERTIES LABELS "r2-desktop-pacing" TIMEOUT 180
                ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
            gtest_discover_tests(engine-sim-pacing-test TEST_FILTER "RuntimeDesktop.*" DISCOVERY_TIMEOUT 30
                WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/pacing-working"
                PROPERTIES LABELS "r2-desktop" TIMEOUT 180
                ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
        endif ()
    endif ()
endif ()

if (ENGINE_SIM_BUILD_CAPTURE)
    add_subdirectory(tools/capture)
endif ()
if (ENGINE_SIM_BUILD_RUNTIME)
    set(engine_sim_runtime_kind STATIC)
    if (ENGINE_SIM_RUNTIME_SHARED)
        set(engine_sim_runtime_kind SHARED)
    endif ()
    add_library(engine-sim-runtime ${engine_sim_runtime_kind} src/runtime/api.cpp)
    add_library(EngineSim::Runtime ALIAS engine-sim-runtime)
    target_include_directories(engine-sim-runtime PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
    target_link_libraries(engine-sim-runtime PRIVATE engine-sim-runtime-backend engine-sim-script-interpreter)
    target_compile_features(engine-sim-runtime PUBLIC cxx_std_17)
    engine_sim_diagnose(engine-sim-runtime TRUE)
endif ()
if (BUILD_TESTING AND (ENGINE_SIM_BUILD_CAPTURE OR ENGINE_SIM_BUILD_DESKTOP OR ENGINE_SIM_BUILD_RUNTIME))
    enable_language(C)
    add_subdirectory(test/runtime)
endif ()

foreach(target IN ITEMS engine-sim engine-sim-host-resources engine-sim-desktop-run-test engine-sim-sdl-audio engine-sim-sdl-audio-test engine-sim-sdl-renderer engine-sim-sdl-render-test engine-sim-sdl-window engine-sim-sdl-window-test engine-sim-host engine-sim-host-test engine-sim-host-application-test engine-sim-frontend engine-sim-script-interpreter engine-sim-app engine-sim-test
        engine-sim-threaded-test engine-sim-script-test engine-sim-lifecycle-test
        engine-sim-displacement-test engine-sim-load-test engine-sim-wave-test engine-sim-resource-test engine-sim-capture)
    engine_sim_diagnose("${target}" TRUE)
endforeach ()
foreach(target IN ITEMS simple-2d-constraint-solver piranha csv-io gtest gtest_main delta-core delta-basic)
    engine_sim_diagnose("${target}" FALSE)
endforeach ()

if (PIRANHA_ENABLED AND CMAKE_EXPORT_COMPILE_COMMANDS AND NOT MSVC)
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    find_program(ENGINE_SIM_ANALYZER NAMES clang++-19 clang++-18 clang++)
    if (ENGINE_SIM_ANALYZER)
        set(engine_sim_analysis_frontend)
        if (TARGET engine-sim-frontend)
            set(engine_sim_analysis_frontend --frontend)
        elseif (TARGET engine-sim-runtime)
            set(engine_sim_analysis_frontend --runtime)
        endif ()
        if (TARGET engine-sim-capture)
            list(APPEND engine_sim_analysis_frontend --capture)
        endif ()
        if (TARGET engine-sim-sdl-window)
            list(APPEND engine_sim_analysis_frontend --sdl-window)
        endif ()
        if (TARGET engine-sim-sdl-renderer)
            list(APPEND engine_sim_analysis_frontend --sdl-renderer)
        endif ()
        if (TARGET engine-sim-sdl-audio)
            list(APPEND engine_sim_analysis_frontend --sdl-audio)
        endif ()
        if (TARGET engine-sim-app AND NOT WIN32)
            list(APPEND engine_sim_analysis_frontend --sdl-host)
        endif ()
        add_custom_target(engine-sim-analysis
            COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/cmake/run_analysis.py"
                --database "${PROJECT_BINARY_DIR}/compile_commands.json"
                --out "${PROJECT_BINARY_DIR}/analysis" --compiler "${ENGINE_SIM_ANALYZER}" ${engine_sim_analysis_frontend}
            VERBATIM)
    endif ()
endif ()

include(cmake/RuntimeInstall.cmake)
if (TARGET engine-sim-runtime)
    include(cmake/RuntimeSdk.cmake)
endif ()
if (ENGINE_SIM_BUILD_RUNTIME_EXAMPLES)
    add_subdirectory(examples/runtime)
endif ()
if (TARGET engine-sim-render-json-test AND TARGET engine-sim-render AND TARGET engine-sim-config-roundtrip)
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    set(preset_check "${PROJECT_SOURCE_DIR}/test/config/preset_corpus.py")
    add_executable(engine-sim-preset-runtime-probe test/config/preset_runtime_probe.cpp)
    target_include_directories(engine-sim-preset-runtime-probe PRIVATE "${PROJECT_SOURCE_DIR}/tools/capture")
    target_link_libraries(engine-sim-preset-runtime-probe PRIVATE
        engine-sim-runtime-json-private engine-sim-script-interpreter)
    engine_sim_diagnose(engine-sim-preset-runtime-probe TRUE)

    function(engine_sim_check_presets prefix behavior index count)
        add_test(NAME "${prefix}PresetCoverage"
            COMMAND "${Python3_EXECUTABLE}" -B "${preset_check}" coverage "${PROJECT_SOURCE_DIR}"
                "${PROJECT_SOURCE_DIR}/${index}" "${count}" ${ARGN} --writer "$<TARGET_FILE:engine-sim-config-roundtrip>")
        add_test(NAME "${behavior}"
            COMMAND "${Python3_EXECUTABLE}" -B "${preset_check}" behavior "${PROJECT_SOURCE_DIR}"
                "${PROJECT_SOURCE_DIR}/${index}" "${count}" ${ARGN} --legacy "$<TARGET_FILE:engine-sim-render>"
                --writer "$<TARGET_FILE:engine-sim-config-roundtrip>"
                --renderer "$<TARGET_FILE:engine-sim-render-json-test>"
                --probe "$<TARGET_FILE:engine-sim-preset-runtime-probe>")
        set_tests_properties("${prefix}PresetCoverage" "${behavior}" PROPERTIES LABELS r25-corpus TIMEOUT 180)
    endfunction()
    engine_sim_check_presets(VideoOne VideoOneBehavior assets/engines/atg-video-1/presets.json 8 --numbered)
    engine_sim_check_presets(VideoTwoSix HeaderAndIgnitionParity assets/engines/atg-video-2/presets-01-06.json 6 --numbered)
    engine_sim_check_presets(Large RadialAndBankOrderParity assets/engines/atg-video-2/presets-07-12.json 6
        --numbered --first-number 7)
    engine_sim_check_presets(Historical DefaultSelectionAndThemes assets/presets-historical.json 6
        --catalog "${PROJECT_SOURCE_DIR}/assets/preset-catalog.json")
endif ()
