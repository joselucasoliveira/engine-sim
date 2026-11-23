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

add_executable(engine-sim-config-graph-test graph_validation.cpp)
target_link_libraries(engine-sim-config-graph-test PRIVATE EngineSim::Config)
set_target_properties(engine-sim-config-graph-test PROPERTIES
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-config-graph-test TRUE)
endif ()
add_test(NAME ConfigReferenceRules COMMAND engine-sim-config-graph-test references)
add_test(NAME ConfigTopologyRules COMMAND engine-sim-config-graph-test topology)
set_tests_properties(ConfigReferenceRules ConfigTopologyRules PROPERTIES LABELS r25-config TIMEOUT 60)

if (TARGET engine-sim-json)
    add_executable(engine-sim-json-lexical-test json_lexical.cpp)
    target_link_libraries(engine-sim-json-lexical-test PRIVATE EngineSim::Json EngineSim::Config)
    target_include_directories(engine-sim-json-lexical-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/json/src")
    set_target_properties(engine-sim-json-lexical-test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if (COMMAND engine_sim_diagnose)
        engine_sim_diagnose(engine-sim-json-lexical-test TRUE)
    endif ()
    add_test(NAME ConfigJsonLexical COMMAND engine-sim-json-lexical-test lexical)
    add_test(NAME ConfigJsonLimits COMMAND engine-sim-json-lexical-test limits)
    set_tests_properties(ConfigJsonLexical ConfigJsonLimits PROPERTIES LABELS r25-config TIMEOUT 60)
endif ()

if (TARGET engine-sim-json)
    add_executable(engine-sim-config-schema-test schema_cases.cpp)
    target_link_libraries(engine-sim-config-schema-test PRIVATE EngineSim::Json EngineSim::Config)
    target_include_directories(engine-sim-config-schema-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/json/src")
    set_target_properties(engine-sim-config-schema-test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if (COMMAND engine_sim_diagnose)
        engine_sim_diagnose(engine-sim-config-schema-test TRUE)
    endif ()
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_test(NAME ConfigSchemaParity COMMAND engine-sim-config-schema-test parity
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/schema-engine.json")
    add_test(NAME ConfigSchemaEmbedding COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/schema_embedding.py" "$<TARGET_FILE:engine-sim-config-schema-test>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/compile_config_schema.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../schemas/engine-config-v1.schema.json")
    set_tests_properties(ConfigSchemaParity ConfigSchemaEmbedding PROPERTIES LABELS r25-config TIMEOUT 60)
endif ()

if (TARGET engine-sim-json AND TARGET engine-sim-config-roundtrip)
    add_executable(engine-sim-config-json-roundtrip-test json_roundtrip.cpp)
    target_link_libraries(engine-sim-config-json-roundtrip-test PRIVATE EngineSim::Json)
    target_include_directories(engine-sim-config-json-roundtrip-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../modules/json/src")
    set_target_properties(engine-sim-config-json-roundtrip-test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if (COMMAND engine_sim_diagnose)
        engine_sim_diagnose(engine-sim-config-json-roundtrip-test TRUE)
    endif ()
    add_test(NAME ConfigJsonRoundTrip COMMAND engine-sim-config-json-roundtrip-test
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/schema-engine.json")
    add_test(NAME ConfigJsonDeterminism COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/json_determinism.py" "$<TARGET_FILE:engine-sim-config-roundtrip>"
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/schema-engine.json")
    set_tests_properties(ConfigJsonRoundTrip ConfigJsonDeterminism PROPERTIES LABELS r25-config TIMEOUT 60)
endif ()

if (TARGET engine-sim-assembly)
    add_executable(engine-sim-mechanical-assembly-test mechanical_assembly.cpp)
    target_link_libraries(engine-sim-mechanical-assembly-test PRIVATE engine-sim-assembly engine-sim EngineSim::Json)
    target_include_directories(engine-sim-mechanical-assembly-test PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/modules/assembly/src")
    set_target_properties(engine-sim-mechanical-assembly-test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    engine_sim_diagnose(engine-sim-mechanical-assembly-test TRUE)
    set(assembly_fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/schema-engine.json")
    add_test(NAME CanonicalMechanicalAssembly COMMAND engine-sim-mechanical-assembly-test assembly "${assembly_fixture}")
    add_test(NAME CanonicalRodPlacement COMMAND engine-sim-mechanical-assembly-test placement "${assembly_fixture}")
    set_tests_properties(CanonicalMechanicalAssembly CanonicalRodPlacement PROPERTIES LABELS r25-assembly TIMEOUT 60)

    add_executable(engine-sim-simulation-assembly-test simulation_assembly.cpp)
    target_link_libraries(engine-sim-simulation-assembly-test PRIVATE engine-sim-assembly engine-sim EngineSim::Json)
    target_include_directories(engine-sim-simulation-assembly-test PRIVATE
        "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/modules/assembly/src")
    set_target_properties(engine-sim-simulation-assembly-test PROPERTIES
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    engine_sim_diagnose(engine-sim-simulation-assembly-test TRUE)
    add_test(NAME CanonicalInductionAssembly COMMAND engine-sim-simulation-assembly-test induction "${assembly_fixture}")
    add_test(NAME CanonicalInitialization COMMAND engine-sim-simulation-assembly-test initialization "${assembly_fixture}")
    set_tests_properties(CanonicalInductionAssembly CanonicalInitialization PROPERTIES LABELS r25-assembly TIMEOUT 60)
endif ()
