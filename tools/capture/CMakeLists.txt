find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD HEAD^{tree}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE source_identity)
execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks diff --binary HEAD
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY
    OUTPUT_VARIABLE source_delta)
string(SHA256 patch_hash "${source_delta}")

configure_file(capture_config.h.in capture_config.h @ONLY)
configure_file(capture_cli.py capture_cli.py COPYONLY)
configure_file(capture_metrics.py capture_metrics.py COPYONLY)
file(CONFIGURE OUTPUT build.txt CONTENT
    "${source_identity}${patch_hash}\n${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\n${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}\n${CMAKE_BUILD_TYPE}\n"
    @ONLY)

add_executable(engine-sim-capture launcher.cpp worker.cpp)
target_include_directories(engine-sim-capture PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(engine-sim-capture PRIVATE engine-sim-script-interpreter)
foreach(configuration IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${configuration}" config_upper)
    set_target_properties(engine-sim-capture PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endforeach ()

if (BUILD_TESTING)
    add_test(NAME CaptureComparator
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_BINARY_DIR}/capture_cli.py"
        "$<TARGET_FILE:engine-sim-capture>" "${CMAKE_CURRENT_BINARY_DIR}/build.txt"
        "${PROJECT_SOURCE_DIR}" --self-test)
    add_test(NAME CaptureQuick
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_BINARY_DIR}/capture_cli.py"
        "$<TARGET_FILE:engine-sim-capture>" "${CMAKE_CURRENT_BINARY_DIR}/build.txt"
        "${PROJECT_SOURCE_DIR}" --quick-test)
    set_tests_properties(CaptureComparator CaptureQuick PROPERTIES LABELS characterization TIMEOUT 90)
endif ()
