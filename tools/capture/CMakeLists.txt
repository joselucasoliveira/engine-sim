find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD HEAD^{tree}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE source_identity)
execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks diff --binary HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE source_delta)
string(SHA256 patch_hash "${source_delta}")

configure_file(capture_cli.py capture_cli.py COPYONLY)
configure_file(capture_metrics.py capture_metrics.py COPYONLY)
file(CONFIGURE OUTPUT build.txt CONTENT
    "${source_identity}${patch_hash}\n${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}\n${CMAKE_BUILD_TYPE}\n"
    @ONLY)

add_library(engine-sim-capture-runtime STATIC "${PROJECT_SOURCE_DIR}/src/runtime/capture_runtime.cpp")
target_link_libraries(engine-sim-capture-runtime PUBLIC engine-sim-runtime-json-private engine-sim-runtime-backend)
engine_sim_diagnose(engine-sim-capture-runtime TRUE)
add_executable(engine-sim-capture launcher.cpp worker.cpp)
if (WIN32)
    target_sources(engine-sim-capture PRIVATE "${PROJECT_SOURCE_DIR}/src/app.manifest")
endif ()
target_include_directories(engine-sim-capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(engine-sim-capture PRIVATE engine-sim-capture-runtime)
if (TARGET engine-sim-host-resources)
    target_link_libraries(engine-sim-capture PRIVATE engine-sim-host-resources)
endif ()
foreach(configuration IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" config_upper)
    set_target_properties(engine-sim-capture PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endforeach ()

if (BUILD_TESTING)
    set(canonical_capture_gate "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/test/capture_canonical_gate.py")
    set(canonical_capture_args --source "${PROJECT_SOURCE_DIR}" --capture "$<TARGET_FILE:engine-sim-capture>"
        --helpers "${CMAKE_CURRENT_BINARY_DIR}")
    if (CMAKE_NM)
        list(APPEND canonical_capture_args --nm "${CMAKE_NM}")
    endif ()
    add_test(NAME CaptureCanonicalReplay COMMAND ${canonical_capture_gate} replay ${canonical_capture_args})
    add_test(NAME CaptureNoLegacyAdapter COMMAND ${canonical_capture_gate} script_free ${canonical_capture_args})
    set_tests_properties(CaptureCanonicalReplay CaptureNoLegacyAdapter PROPERTIES LABELS r25-capture TIMEOUT 180)

    add_executable(engine-sim-capture-test "${PROJECT_SOURCE_DIR}/test/capture_runtime_tests.cpp")
    target_link_libraries(engine-sim-capture-test PRIVATE engine-sim-capture-runtime gtest_main)
    engine_sim_diagnose(engine-sim-capture-test TRUE)
    gtest_discover_tests(engine-sim-capture-test DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "r2-capture" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
    add_test(NAME CaptureComparator
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_BINARY_DIR}/capture_cli.py"
        "$<TARGET_FILE:engine-sim-capture>" "${CMAKE_CURRENT_BINARY_DIR}/build.txt"
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" --self-test)
    add_test(NAME CaptureQuick
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_BINARY_DIR}/capture_cli.py"
        "$<TARGET_FILE:engine-sim-capture>" "${CMAKE_CURRENT_BINARY_DIR}/build.txt"
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" --quick-test)
    set_tests_properties(CaptureComparator CaptureQuick PROPERTIES LABELS "characterization;r2-oracle;r2-capture" TIMEOUT 90)
    add_test(NAME CaptureMechanics
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_BINARY_DIR}/capture_cli.py"
        "$<TARGET_FILE:engine-sim-capture>" "${CMAKE_CURRENT_BINARY_DIR}/build.txt"
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" --mechanics-test)
    set_tests_properties(CaptureMechanics PROPERTIES LABELS "characterization;r2-oracle;r2-capture" TIMEOUT 90)
    add_test(NAME CaptureStress
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_BINARY_DIR}/capture_cli.py"
        "$<TARGET_FILE:engine-sim-capture>" "${CMAKE_CURRENT_BINARY_DIR}/build.txt"
        "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}" --stress-test)
    set_tests_properties(CaptureStress PROPERTIES LABELS "stress;r2-capture" TIMEOUT 240)
    add_test(NAME StagedResources
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/test/staged_resources.py"
        --cmake "${CMAKE_COMMAND}" --build "${PROJECT_BINARY_DIR}"
        --source "${PROJECT_SOURCE_DIR}" --config "$<CONFIG>")
    set_tests_properties(StagedResources PROPERTIES LABELS resources TIMEOUT 240)
endif ()
