add_library(engine-sim-config INTERFACE)
add_library(EngineSim::Config ALIAS engine-sim-config)
target_include_directories(engine-sim-config INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(engine-sim-config INTERFACE cxx_std_17)
