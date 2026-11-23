add_executable(engine-sim-script-test script_tests.cpp)
target_link_libraries(engine-sim-script-test PRIVATE engine-sim-script-interpreter gtest_main)

set(fixtures "${CMAKE_CURRENT_BINARY_DIR}/input scripts")
file(GENERATE OUTPUT "${fixtures}/library.mr" CONTENT [=[
import "nested/inner.mr"
]=])
file(GENERATE OUTPUT "${fixtures}/nested/inner.mr" CONTENT [=[
import "engine_sim.mr"
]=])
file(GENERATE OUTPUT "${fixtures}/unknown-node.mr" CONTENT [=[
import "engine_sim.mr"
nonexistent_test_node()
]=])
file(GENERATE OUTPUT "${fixtures}/missing-import.mr" CONTENT [=[
import "nonexistent_module_test.mr"
]=])

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/working")
gtest_discover_tests(engine-sim-script-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/working"
    DISCOVERY_TIMEOUT 30
    PROPERTIES
        LABELS script
)

add_executable(engine-sim-lifecycle-test lifecycle_tests.cpp)
target_link_libraries(engine-sim-lifecycle-test PRIVATE engine-sim-script-interpreter gtest_main)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-working")
gtest_discover_tests(engine-sim-lifecycle-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-working"
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS lifecycle TIMEOUT 600 ENVIRONMENT "ENGINE_SIM_TEST_RESOURCES=${PROJECT_SOURCE_DIR}")

add_executable(engine-sim-displacement-test displacement_tests.cpp)
target_link_libraries(engine-sim-displacement-test PRIVATE engine-sim-script-interpreter gtest_main)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/displacement-working")
gtest_discover_tests(engine-sim-displacement-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/displacement-working"
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS displacement TIMEOUT 90 ENVIRONMENT "ENGINE_SIM_TEST_RESOURCES=${PROJECT_SOURCE_DIR}")

add_executable(engine-sim-load-test load_tests.cpp)
target_link_libraries(engine-sim-load-test PRIVATE engine-sim-script-interpreter gtest_main)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/load-working")
gtest_discover_tests(engine-sim-load-test
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/load-working"
    DISCOVERY_TIMEOUT 30
    PROPERTIES LABELS load TIMEOUT 600 ENVIRONMENT "ENGINE_SIM_TEST_RESOURCES=${PROJECT_SOURCE_DIR}")

configure_file(discovered_properties.cmake.in discovered_properties.cmake @ONLY)
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${CMAKE_CURRENT_BINARY_DIR}/discovered_properties.cmake")

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
add_executable(engine-sim-parser-preparation-test preparation_tests.cpp)
target_link_libraries(engine-sim-parser-preparation-test PRIVATE engine-sim-script-interpreter)
if (TARGET engine-sim-host-resources)
    target_link_libraries(engine-sim-parser-preparation-test PRIVATE engine-sim-host-resources)
endif ()
engine_sim_diagnose(engine-sim-parser-preparation-test TRUE)
add_test(NAME RuntimeParserPreparation
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/preparation_probe.py"
        "$<TARGET_FILE:engine-sim-parser-preparation-test>" "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(RuntimeParserPreparation PROPERTIES LABELS "r2-parser;script" TIMEOUT 300)

if (TARGET engine-sim-legacy-snapshot)
    add_executable(engine-sim-legacy-capture-test legacy_capture.cpp)
    target_link_libraries(engine-sim-legacy-capture-test
        PRIVATE engine-sim-legacy-snapshot EngineSim::Json)
    engine_sim_diagnose(engine-sim-legacy-capture-test TRUE)

    set(capture_script "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/legacy_capture.mr")
    set(capture_resources "${PROJECT_SOURCE_DIR}/examples/runtime/fixture")
    set(capture_missing "${CMAKE_CURRENT_BINARY_DIR}/legacy-capture-missing.mr")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${capture_script}")
    file(READ "${capture_script}" capture_input)
    string(REPLACE "motor.add_ignition_module(ignition)\n" "" capture_input "${capture_input}")
    file(WRITE "${capture_missing}" "${capture_input}")

    add_test(NAME LegacyEvaluatedValues COMMAND engine-sim-legacy-capture-test values
        "${capture_script}" "${capture_resources}/library" "${capture_resources}" "${capture_missing}")
    add_test(NAME LegacyCurveIdentity COMMAND engine-sim-legacy-capture-test identity
        "${capture_script}" "${capture_resources}/library" "${capture_resources}" "${capture_missing}")
    set_tests_properties(LegacyEvaluatedValues LegacyCurveIdentity PROPERTIES LABELS "r25-convert;script" TIMEOUT 60)
endif ()

if (TARGET engine-sim-legacy-snapshot)
    add_executable(engine-sim-legacy-routes-test legacy_routes.cpp)
    target_link_libraries(engine-sim-legacy-routes-test PRIVATE engine-sim-legacy-snapshot EngineSim::Json)
    engine_sim_diagnose(engine-sim-legacy-routes-test TRUE)
    set(legacy_route_arguments "${CMAKE_CURRENT_SOURCE_DIR}/legacy_routes.py"
        "$<TARGET_FILE:engine-sim-legacy-routes-test>"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/legacy_routes.mr"
        "${PROJECT_SOURCE_DIR}/examples/runtime/fixture/library" "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME LegacySelectionSnapshot COMMAND "${Python3_EXECUTABLE}" -B ${legacy_route_arguments} selection)
    add_test(NAME LegacyActionRouting COMMAND "${Python3_EXECUTABLE}" -B ${legacy_route_arguments} routing)
    set_tests_properties(LegacySelectionSnapshot LegacyActionRouting PROPERTIES LABELS "r25-convert;script" TIMEOUT 90)
endif ()

if (TARGET engine-sim-convert)
    set(legacy_cli_arguments "${CMAKE_CURRENT_SOURCE_DIR}/legacy_converter.py"
        "$<TARGET_FILE:engine-sim-convert>" "${PROJECT_SOURCE_DIR}/examples/runtime/fixture"
        "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME LegacyConverterCli COMMAND "${Python3_EXECUTABLE}" -B ${legacy_cli_arguments} cli)
    add_test(NAME LegacyBatchFailures COMMAND "${Python3_EXECUTABLE}" -B ${legacy_cli_arguments} batch)
    set_tests_properties(LegacyConverterCli LegacyBatchFailures PROPERTIES LABELS "r25-convert;script" TIMEOUT 120)
endif ()
