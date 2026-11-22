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
engine_sim_diagnose(engine-sim-parser-preparation-test TRUE)
add_test(NAME RuntimeParserPreparation
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/preparation_probe.py"
        "$<TARGET_FILE:engine-sim-parser-preparation-test>" "${PROJECT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(RuntimeParserPreparation PROPERTIES LABELS "r2-parser;script" TIMEOUT 300)
