find_package(Threads REQUIRED)

set(synthesis_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_library(engine-sim-synthesis-objects OBJECT
    "${synthesis_root}/src/synthesizer.cpp"
    "${synthesis_root}/src/convolution_filter.cpp"
    "${synthesis_root}/src/derivative_filter.cpp"
    "${synthesis_root}/src/filter.cpp"
    "${synthesis_root}/src/jitter_filter.cpp"
    "${synthesis_root}/src/leveling_filter.cpp"
    "${synthesis_root}/src/low_pass_filter.cpp")
target_compile_features(engine-sim-synthesis-objects PRIVATE cxx_std_17)
target_link_libraries(engine-sim-synthesis-objects PRIVATE Threads::Threads)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-synthesis-objects TRUE)
endif ()

add_library(engine-sim-synthesis STATIC $<TARGET_OBJECTS:engine-sim-synthesis-objects>)
target_include_directories(engine-sim-synthesis PUBLIC "${synthesis_root}/include")
target_link_libraries(engine-sim-synthesis PRIVATE Threads::Threads)
target_compile_features(engine-sim-synthesis PUBLIC cxx_std_17)
