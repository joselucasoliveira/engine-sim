find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

set(overlay_sources
    modules/json/src/decode.cpp
    modules/json/src/schema.cpp
    modules/config/src/config.cpp
    modules/assembly/src/preparation.cpp
    src/runtime/json_runtime.cpp
    src/engine.cpp
    src/function.cpp
    src/synthesizer.cpp)
set(overlay_dir "${CMAKE_CURRENT_BINARY_DIR}/overlay")
foreach(path IN LISTS overlay_sources)
    get_filename_component(name "${path}" NAME)
    list(APPEND generated "${overlay_dir}/${name}")
    list(APPEND overlay_inputs "${PROJECT_SOURCE_DIR}/${path}")
endforeach()
add_custom_command(
    OUTPUT ${generated} "${overlay_dir}/identities.json"
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/overlay.py"
        "${PROJECT_SOURCE_DIR}" "${overlay_dir}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/overlay.py" ${overlay_inputs}
    VERBATIM)
add_executable(engine-sim-runtime-phase-bench main.cpp observe.cpp ${generated}
    "${PROJECT_SOURCE_DIR}/src/runtime/api.cpp")
get_target_property(json_build engine-sim-json BINARY_DIR)
target_include_directories(engine-sim-runtime-phase-bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/runtime"
    "${PROJECT_SOURCE_DIR}/modules/assembly/src"
    "${PROJECT_SOURCE_DIR}/modules/config/src"
    "${PROJECT_SOURCE_DIR}/modules/json/src"
    "${json_build}/generated")
target_include_directories(engine-sim-runtime-phase-bench SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/modules/json/third_party/nlohmann")
target_link_libraries(engine-sim-runtime-phase-bench PRIVATE
    engine-sim-runtime-backend engine-sim-assembly EngineSim::Json)
target_compile_definitions(engine-sim-runtime-phase-bench PRIVATE ES_RUNTIME_STATIC)
set_target_properties(engine-sim-runtime-phase-bench PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_compile_definitions(engine-sim-runtime-phase-bench PRIVATE ES_COST_WRAP_PTHREAD)
    target_link_options(engine-sim-runtime-phase-bench PRIVATE "-Wl,--wrap=pthread_mutex_lock")
elseif(WIN32)
    target_link_libraries(engine-sim-runtime-phase-bench PRIVATE psapi)
    if(MINGW)
        target_link_options(engine-sim-runtime-phase-bench PRIVATE -municode)
    endif()
endif()
engine_sim_diagnose(engine-sim-runtime-phase-bench TRUE)
