# The ordinary bootstrap archive stays untouched. Diagnostic runtime profiles
# rebuild both private Boost runtime archives in their own external build tree.
set(engine_sim_boost_filesystem Boost::filesystem)
if (ENGINE_SIM_BUILD_RUNTIME AND UNIX AND NOT ENGINE_SIM_SANITIZER STREQUAL "none")
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    set(diagnostic_boost "${PROJECT_BINARY_DIR}/diagnostic-boost")
    set(diagnostic_filesystem "${diagnostic_boost}/stage/lib/libboost_filesystem.a")
    set(diagnostic_atomic "${diagnostic_boost}/stage/lib/libboost_atomic.a")
    add_custom_command(OUTPUT "${diagnostic_filesystem}" "${diagnostic_atomic}"
        COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/cmake/build_diagnostic_boost.py"
            --root "${BOOST_ROOT}" --out "${diagnostic_boost}"
            --compiler "${CMAKE_CXX_COMPILER}" --sanitizer "${ENGINE_SIM_SANITIZER}"
        DEPENDS "${PROJECT_SOURCE_DIR}/cmake/build_diagnostic_boost.py"
            "${PROJECT_SOURCE_DIR}/cmake/dependencies.lock.json"
        VERBATIM)
    add_custom_target(engine-sim-build-diagnostic-boost DEPENDS "${diagnostic_filesystem}" "${diagnostic_atomic}")
    add_library(engine-sim-boost-diagnostics INTERFACE)
    add_dependencies(engine-sim-boost-diagnostics engine-sim-build-diagnostic-boost)
    target_include_directories(engine-sim-boost-diagnostics INTERFACE "${BOOST_ROOT}")
    target_compile_definitions(engine-sim-boost-diagnostics INTERFACE BOOST_FILESYSTEM_NO_LIB BOOST_FILESYSTEM_STATIC_LINK)
    target_link_libraries(engine-sim-boost-diagnostics INTERFACE "${diagnostic_filesystem}" "${diagnostic_atomic}")
    set(engine_sim_boost_filesystem engine-sim-boost-diagnostics)
endif ()
