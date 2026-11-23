set(json_vendor "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann")
set(json_lock "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/dependencies.lock.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${json_lock}" "${json_vendor}/json.hpp" "${json_vendor}/LICENSE.MIT")
file(READ "${json_lock}" json_lock_bytes)

function(check_json_vendor name key)
    string(JSON expected GET "${json_lock_bytes}" vendored nlohmann-json "${key}")
    file(SHA256 "${json_vendor}/${name}" actual)
    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "Hash mismatch for the locked nlohmann/json ${name}")
    endif ()
endfunction()
check_json_vendor(json.hpp header_sha256)
check_json_vendor(LICENSE.MIT license_sha256)

set(schema "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/engine-config-v1.schema.json")
set(schema_compiler "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/compile_config_schema.py")
set(schema_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(schema_digest "3b084785637e024c7d74e7b567b64b01f4fc620367d978181a24f40b012237ef")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${schema}" "${schema_compiler}")
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${schema_compiler}" "${schema}"
        "${schema_generated}/config_schema_v1.hpp" --expected-sha256 "${schema_digest}"
    COMMAND_ERROR_IS_FATAL ANY)

add_library(engine-sim-json STATIC src/lexical.cpp src/schema.cpp src/decode.cpp src/write.cpp)
target_include_directories(engine-sim-json PRIVATE "${schema_generated}")
add_library(EngineSim::Json ALIAS engine-sim-json)
target_include_directories(engine-sim-json SYSTEM PRIVATE "${json_vendor}")
target_include_directories(engine-sim-json PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_features(engine-sim-json PUBLIC cxx_std_17)
target_link_libraries(engine-sim-json PUBLIC EngineSim::Config)
set_target_properties(engine-sim-json PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-json TRUE)
endif ()
