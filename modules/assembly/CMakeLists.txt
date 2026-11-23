add_library(engine-sim-assembly STATIC
    $<TARGET_OBJECTS:engine-sim-physics-builders> src/preparation.cpp)
target_link_libraries(engine-sim-assembly PUBLIC EngineSim::Config PRIVATE engine-sim)
target_include_directories(engine-sim-assembly PRIVATE "${PROJECT_SOURCE_DIR}/include")
set_target_properties(engine-sim-assembly PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
engine_sim_diagnose(engine-sim-assembly TRUE)
