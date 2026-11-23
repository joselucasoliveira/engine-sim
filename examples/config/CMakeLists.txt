cmake_minimum_required(VERSION 3.25)

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(EngineSimConfigRoundTrip LANGUAGES CXX)
    add_subdirectory(../../modules/config config)
    add_subdirectory(../../modules/json json)
endif ()

add_executable(engine-sim-config-roundtrip roundtrip.cpp)
target_link_libraries(engine-sim-config-roundtrip PRIVATE EngineSim::Json)
set_target_properties(engine-sim-config-roundtrip PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-config-roundtrip TRUE)
endif ()
