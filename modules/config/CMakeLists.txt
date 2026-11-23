add_library(engine-sim-config STATIC
    src/config.cpp
    src/graph.cpp
    src/normalization.cpp
    src/shape.cpp)
add_library(EngineSim::Config ALIAS engine-sim-config)
target_include_directories(engine-sim-config PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(engine-sim-config PUBLIC cxx_std_17)
set_target_properties(engine-sim-config PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-config TRUE)
endif ()
