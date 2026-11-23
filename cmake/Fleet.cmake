add_library(engine-sim-fleet STATIC "${PROJECT_SOURCE_DIR}/src/fleet/owner.cpp")
add_library(EngineSim::Fleet ALIAS engine-sim-fleet)
target_include_directories(engine-sim-fleet
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE "${PROJECT_SOURCE_DIR}/modules/assembly/src" "${PROJECT_SOURCE_DIR}/src/runtime")
target_link_libraries(engine-sim-fleet PRIVATE
    engine-sim-runtime-json-private engine-sim-runtime-backend engine-sim-assembly)
target_compile_definitions(engine-sim-fleet PUBLIC ES_FLEET_STATIC)
target_compile_features(engine-sim-fleet PRIVATE cxx_std_17)
set_target_properties(engine-sim-fleet PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES POSITION_INDEPENDENT_CODE ON)
engine_sim_diagnose(engine-sim-fleet TRUE)
