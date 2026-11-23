cmake_minimum_required(VERSION 3.25)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(EngineSimConfigConsumer LANGUAGES CXX)
    enable_testing()
    add_subdirectory(../../modules/config model)
endif ()

add_executable(engine-sim-config-values-test value_ownership.cpp)
add_executable(engine-sim-config-header-test public_header.cpp)

foreach(target IN ITEMS engine-sim-config-values-test engine-sim-config-header-test)
    target_link_libraries(${target} PRIVATE EngineSim::Config)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if (COMMAND engine_sim_diagnose)
        engine_sim_diagnose(${target} TRUE)
    endif ()
endforeach ()

add_test(NAME ConfigValueOwnership COMMAND engine-sim-config-values-test)
add_test(NAME ConfigPublicHeader COMMAND engine-sim-config-header-test)
set_tests_properties(ConfigValueOwnership ConfigPublicHeader PROPERTIES LABELS r25-config TIMEOUT 60)

add_executable(engine-sim-config-normalization-test normalization.cpp)
target_link_libraries(engine-sim-config-normalization-test PRIVATE EngineSim::Config)
target_include_directories(engine-sim-config-normalization-test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/config/src")
set_target_properties(engine-sim-config-normalization-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-config-normalization-test TRUE)
endif ()
add_test(NAME ConfigDefaults COMMAND engine-sim-config-normalization-test defaults)
add_test(NAME ConfigUnitBoundaries COMMAND engine-sim-config-normalization-test units)
set_tests_properties(ConfigDefaults ConfigUnitBoundaries PROPERTIES LABELS r25-config TIMEOUT 60)
