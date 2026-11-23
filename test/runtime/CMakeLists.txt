function(configure_runtime_abi_check target)
    target_include_directories("${target}" PRIVATE "${PROJECT_SOURCE_DIR}/include")
    set_target_properties("${target}" PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if (MSVC)
        target_compile_options("${target}" PRIVATE /W4 /WX "$<$<COMPILE_LANGUAGE:CXX>:/permissive->")
    elseif (CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$"
            AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options("${target}" PRIVATE -Wall -Wextra -Werror -pedantic-errors)
    endif ()
endfunction()

add_executable(engine-sim-c-abi-check abi_c.c)
add_executable(engine-sim-cxx-abi-check abi_cpp.cpp)
configure_runtime_abi_check(engine-sim-c-abi-check)
configure_runtime_abi_check(engine-sim-cxx-abi-check)
target_compile_definitions(engine-sim-cxx-abi-check PRIVATE ES_RUNTIME_STATIC)

if (WIN32)
    add_library(engine-sim-c-abi-export-check OBJECT abi_c.c)
    configure_runtime_abi_check(engine-sim-c-abi-export-check)
    target_compile_definitions(engine-sim-c-abi-export-check PRIVATE ES_RUNTIME_BUILD)
endif ()

add_test(NAME RuntimeCAbi COMMAND engine-sim-c-abi-check)
add_test(NAME RuntimeCxxAbi COMMAND engine-sim-cxx-abi-check)
set_tests_properties(RuntimeCAbi RuntimeCxxAbi PROPERTIES LABELS "r2-abi")

if (TARGET engine-sim-script-interpreter)
    add_executable(engine-sim-preparation-test preparation_tests.cpp)
    target_link_libraries(engine-sim-preparation-test PRIVATE engine-sim-script-interpreter gtest_main)
    engine_sim_diagnose(engine-sim-preparation-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/preparation-working")
    gtest_discover_tests(engine-sim-preparation-test
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/preparation-working"
        DISCOVERY_TIMEOUT 30
        PROPERTIES LABELS "r2-prepare" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")

    add_executable(engine-sim-seed-test seed_tests.cpp)
    target_link_libraries(engine-sim-seed-test PRIVATE engine-sim-script-interpreter gtest_main)
    engine_sim_diagnose(engine-sim-seed-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/seed-working")
    gtest_discover_tests(engine-sim-seed-test
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/seed-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-seed" TIMEOUT 180
        ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")

    add_executable(engine-sim-clock-test clock_tests.cpp)
    target_link_libraries(engine-sim-clock-test PRIVATE engine-sim-script-interpreter gtest_main)
    engine_sim_diagnose(engine-sim-clock-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clock-working")
    gtest_discover_tests(engine-sim-clock-test
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clock-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-clock" TIMEOUT 180
        ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-diagnostics-test diagnostics_tests.cpp)
    target_link_libraries(engine-sim-diagnostics-test PRIVATE EngineSim::Runtime engine-sim-runtime-backend gtest_main)
    target_include_directories(engine-sim-diagnostics-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-diagnostics-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/diagnostics-working")
    gtest_discover_tests(engine-sim-diagnostics-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/diagnostics-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-diagnostics" TIMEOUT 300
        ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")

    add_executable(engine-sim-runtime-c-test consumer_c.c)
    target_link_libraries(engine-sim-runtime-c-test PRIVATE EngineSim::Runtime)
    set_target_properties(engine-sim-runtime-c-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO LINKER_LANGUAGE CXX)
    engine_sim_diagnose(engine-sim-runtime-c-test TRUE)
    set(fixture "${PROJECT_SOURCE_DIR}/examples/runtime/fixture")
    add_test(NAME RuntimeCConsumer COMMAND engine-sim-runtime-c-test "${fixture}/main.json" "${fixture}")
    set_tests_properties(RuntimeCConsumer PROPERTIES LABELS "r2-runtime" TIMEOUT 180)
    add_executable(engine-sim-runtime-test runtime_tests.cpp)
    target_link_libraries(engine-sim-runtime-test PRIVATE EngineSim::Runtime engine-sim-runtime-backend gtest_main)
    if (TARGET engine-sim-script-interpreter)
        target_link_libraries(engine-sim-runtime-test PRIVATE engine-sim-script-interpreter)
        target_compile_definitions(engine-sim-runtime-test PRIVATE ENGINE_SIM_TEST_LEGACY_PREPARATION=1)
    endif ()
    target_include_directories(engine-sim-runtime-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-runtime-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime-working")
    gtest_discover_tests(engine-sim-runtime-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-runtime" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-runtime-lifecycle-test lifecycle_tests.cpp)
    target_link_libraries(engine-sim-runtime-lifecycle-test PRIVATE EngineSim::Runtime engine-sim-runtime-backend gtest_main)
    target_include_directories(engine-sim-runtime-lifecycle-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-runtime-lifecycle-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-working")
    gtest_discover_tests(engine-sim-runtime-lifecycle-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-lifecycle" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-rate-test rate_tests.cpp)
    target_link_libraries(engine-sim-rate-test PRIVATE EngineSim::Runtime engine-sim-runtime-backend gtest_main)
    target_include_directories(engine-sim-rate-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-rate-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rate-working")
    gtest_discover_tests(engine-sim-rate-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rate-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-rate" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-runtime-load-test load_tests.cpp)
    target_link_libraries(engine-sim-runtime-load-test PRIVATE EngineSim::Runtime engine-sim-runtime-backend gtest_main)
    target_include_directories(engine-sim-runtime-load-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-runtime-load-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/load-working")
    gtest_discover_tests(engine-sim-runtime-load-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/load-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-load" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-drive-test drive_tests.cpp)
    target_link_libraries(engine-sim-drive-test PRIVATE EngineSim::Runtime engine-sim-runtime-backend gtest_main)
    target_include_directories(engine-sim-drive-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-drive-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/drive-working")
    gtest_discover_tests(engine-sim-drive-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/drive-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-drive" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    add_test(NAME RuntimeConsumerTools COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/consumer_tool_tests.py"
        --out "${CMAKE_CURRENT_BINARY_DIR}/consumer-tool-working")
    set_tests_properties(RuntimeConsumerTools PROPERTIES LABELS "r2-sdk-tools" TIMEOUT 60)
    add_test(NAME RuntimeCiTools COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/ci_tools_tests.py"
        --out "${CMAKE_CURRENT_BINARY_DIR}/ci-tool-working")
    set_tests_properties(RuntimeCiTools PROPERTIES LABELS "r2-example" TIMEOUT 60)
    if (ENGINE_SIM_SANITIZER STREQUAL "none")
        set(native_gate "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/native_gate.py")
        add_test(NAME NativeMatrixContract COMMAND ${native_gate} matrix
            --out "${CMAKE_CURRENT_BINARY_DIR}/native-matrix-working")
        add_test(NAME QualificationReceiptIntegrity COMMAND ${native_gate} transfer
            --out "${CMAKE_CURRENT_BINARY_DIR}/qualification-receipt-working")
        set_tests_properties(NativeMatrixContract QualificationReceiptIntegrity PROPERTIES
            LABELS r25-native RUN_SERIAL TRUE TIMEOUT 1200)
    endif ()
    # Timings require an optimized, unsanitized static implementation. The probe
    # is a different executable so allocation and stage clocks cannot affect it.
    if (NOT ENGINE_SIM_RUNTIME_SHARED AND ENGINE_SIM_SANITIZER STREQUAL "none")
        foreach(variant IN ITEMS bench probe)
            add_executable(engine-sim-runtime-${variant} benchmark.cpp)
            target_link_libraries(engine-sim-runtime-${variant} PRIVATE EngineSim::Runtime engine-sim-runtime-backend)
            target_include_directories(engine-sim-runtime-${variant} PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
            engine_sim_diagnose(engine-sim-runtime-${variant} TRUE)
            if (WIN32)
                target_link_libraries(engine-sim-runtime-${variant} PRIVATE psapi)
            endif ()
        endforeach ()
        target_compile_definitions(engine-sim-runtime-probe PRIVATE ES_BENCH_PROBE=1)
        if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_compile_definitions(engine-sim-runtime-probe PRIVATE ES_BENCH_MUTEX_WRAP=1)
            target_link_options(engine-sim-runtime-probe PRIVATE "LINKER:--wrap=pthread_mutex_lock")
        endif ()
        add_test(NAME RuntimeBenchmark COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tools/runtime_bench.py"
            --benchmark "$<TARGET_FILE:engine-sim-runtime-bench>" --probe "$<TARGET_FILE:engine-sim-runtime-probe>"
            --fixture "${PROJECT_SOURCE_DIR}/examples/runtime/fixture" --out "${CMAKE_CURRENT_BINARY_DIR}/benchmark-smoke" --smoke
            --compile-database "${CMAKE_BINARY_DIR}/compile_commands.json")
        set_tests_properties(RuntimeBenchmark PROPERTIES LABELS "runtime-bench" TIMEOUT 300)
    endif ()
    if (TARGET engine-sim-runtime AND ENGINE_SIM_SANITIZER STREQUAL "none")
        add_subdirectory(cost-phases)
        set(runtime_cost_gate "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/cost_gate.py")
        set(runtime_cost_inputs --source "${PROJECT_SOURCE_DIR}"
            --fixture "${PROJECT_SOURCE_DIR}/examples/runtime/fixture" --deps-root "${ENGINE_SIM_PRESET_ROOT}")
        add_test(NAME PreparationPhaseAccounting COMMAND ${runtime_cost_gate} phases
            ${runtime_cost_inputs} --executable "$<TARGET_FILE:engine-sim-runtime-phase-bench>"
            --overlay "${CMAKE_CURRENT_BINARY_DIR}/cost-phases/overlay/identities.json")
        add_test(NAME IsolatedRuntimeCosts COMMAND ${runtime_cost_gate} installed
            ${runtime_cost_inputs} --compiler "${CMAKE_CXX_COMPILER}"
            --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}"
            --generator "${CMAKE_GENERATOR}" "--platform=${CMAKE_GENERATOR_PLATFORM}")
        set_tests_properties(PreparationPhaseAccounting IsolatedRuntimeCosts PROPERTIES
            LABELS runtime-bench RUN_SERIAL TRUE TIMEOUT 1200)
    endif ()
    add_test(NAME StandaloneSynthesisConsumer COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/synthesis_projection.py"
        --source "${PROJECT_SOURCE_DIR}" --generator "${CMAKE_GENERATOR}"
        "--platform=${CMAKE_GENERATOR_PLATFORM}" --compiler "${CMAKE_CXX_COMPILER}"
        --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}")
    add_executable(engine-sim-synthesis-stream-test synthesis_stream_test.cpp
        "${PROJECT_SOURCE_DIR}/src/sample_clock.cpp")
    target_link_libraries(engine-sim-synthesis-stream-test PRIVATE engine-sim-synthesis)
    engine_sim_diagnose(engine-sim-synthesis-stream-test TRUE)
    add_test(NAME SynthesisStreamParity COMMAND engine-sim-synthesis-stream-test)
    set_tests_properties(StandaloneSynthesisConsumer SynthesisStreamParity PROPERTIES LABELS r25-synthesis TIMEOUT 120)

    set(physics_projection "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/physics_projection.py"
        --source "${PROJECT_SOURCE_DIR}" --generator "${CMAKE_GENERATOR}"
        "--platform=${CMAKE_GENERATOR_PLATFORM}" --compiler "${CMAKE_CXX_COMPILER}"
        --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}")
    add_test(NAME StandalonePhysicsConsumer COMMAND ${physics_projection})
    add_test(NAME ModuleDependencyDirection COMMAND ${physics_projection} --direction)
    set_tests_properties(StandalonePhysicsConsumer ModuleDependencyDirection PROPERTIES LABELS r25-physics TIMEOUT 180)

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ENGINE_SIM_SANITIZER STREQUAL "none")
        set(profile_gate "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/profile_gate.py"
            --source "${PROJECT_SOURCE_DIR}" --deps-root "${ENGINE_SIM_PRESET_ROOT}"
            --compiler "${CMAKE_CXX_COMPILER}" --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}")
        add_test(NAME ProfileDependencyIsolation COMMAND ${profile_gate} isolation)
        add_test(NAME CombinedProfileClosure COMMAND ${profile_gate} combined)
        set_tests_properties(ProfileDependencyIsolation CombinedProfileClosure PROPERTIES RUN_SERIAL TRUE TIMEOUT 1200)
        set(documentation_gate "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/documentation_gate.py"
            --out "${CMAKE_CURRENT_BINARY_DIR}/documentation-working" --deps-root "${ENGINE_SIM_PRESET_ROOT}")
        add_test(NAME DocumentedQuickstarts COMMAND ${documentation_gate} quickstarts)
        add_test(NAME MigrationGuideExamples COMMAND ${documentation_gate} migration)
        set_tests_properties(DocumentedQuickstarts MigrationGuideExamples PROPERTIES
            LABELS r25-documentation RUN_SERIAL TRUE TIMEOUT 1200)
        set_property(TEST MigrationGuideExamples APPEND PROPERTY LABELS r25-profile-combined)
        set_tests_properties(CombinedProfileClosure PROPERTIES
            DEPENDS ProfileDependencyIsolation LABELS "r25-profile-combined" TIMEOUT 1800)
        set(sdk_relocation "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/relocation_gate.py"
            --source "${PROJECT_SOURCE_DIR}" --deps-root "${ENGINE_SIM_PRESET_ROOT}"
            --compiler "${CMAKE_CXX_COMPILER}" --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}")
        add_test(NAME RelocatedRuntimeStatic COMMAND ${sdk_relocation} --variant static)
        add_test(NAME RelocatedRuntimeShared COMMAND ${sdk_relocation} --variant shared)
        set_tests_properties(RelocatedRuntimeStatic RelocatedRuntimeShared PROPERTIES
            RUN_SERIAL TRUE LABELS r25-sdk-relocation TIMEOUT 1500)
        set(config_consumer "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/programmatic_gate.py"
            --source "${PROJECT_SOURCE_DIR}" --deps-root "${ENGINE_SIM_PRESET_ROOT}"
            --cmake "${CMAKE_COMMAND}" --compiler "${CMAKE_CXX_COMPILER}")
        add_test(NAME InstalledConfigConsumer COMMAND ${config_consumer} model)
        add_test(NAME InstalledProgrammaticEngine COMMAND ${config_consumer} runtime)
        set_tests_properties(InstalledConfigConsumer InstalledProgrammaticEngine PROPERTIES
            RUN_SERIAL TRUE LABELS r25-config-sdk TIMEOUT 600)
    endif ()

    foreach(variant IN ITEMS static shared)
        add_test(NAME RuntimeSdk_${variant} COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/sdk_gate.py"
            --source "${PROJECT_SOURCE_DIR}" --root "${ENGINE_SIM_PRESET_ROOT}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/sdk-admission" --variant "${variant}")
        set_tests_properties(RuntimeSdk_${variant} PROPERTIES LABELS "r2-sdk" RUN_SERIAL TRUE TIMEOUT 600)
    endforeach ()
endif ()

if (TARGET engine-sim-runtime AND ENGINE_SIM_BUILD_RUNTIME_EXAMPLES)
    add_executable(engine-sim-example-reference reference/direct.c)
    target_link_libraries(engine-sim-example-reference PRIVATE EngineSim::Runtime)
    set_target_properties(engine-sim-example-reference PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO LINKER_LANGUAGE CXX)
    engine_sim_diagnose(engine-sim-example-reference TRUE)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ENGINE_SIM_SANITIZER STREQUAL "none")
        set(csv_gate "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/csv_gate.py"
            --source "${PROJECT_SOURCE_DIR}" --deps-root "${ENGINE_SIM_PRESET_ROOT}"
            --compiler "${CMAKE_CXX_COMPILER}" --cmake "${CMAKE_COMMAND}" --ctest "${CMAKE_CTEST_COMMAND}"
            --render "$<TARGET_FILE:engine-sim-render>" --reference "$<TARGET_FILE:engine-sim-example-reference>")
        add_test(NAME CsvFreeBuildProfiles COMMAND ${csv_gate} profiles)
        add_test(NAME CaptureTextFormats COMMAND ${csv_gate} formats)
        set_tests_properties(CsvFreeBuildProfiles PROPERTIES LABELS "r25-profile-combined" RUN_SERIAL TRUE TIMEOUT 900)
        set_tests_properties(CaptureTextFormats PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
    endif ()
    add_test(NAME RuntimeExampleNative COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/example_tests.py"
        --cli "$<TARGET_FILE:engine-sim-render>" --reference "$<TARGET_FILE:engine-sim-example-reference>"
        --fixture "${PROJECT_SOURCE_DIR}/examples/runtime/fixture" --out "${CMAKE_CURRENT_BINARY_DIR}/example-admission")
    set_tests_properties(RuntimeExampleNative PROPERTIES LABELS "r2-example" TIMEOUT 180)
    foreach(variant IN ITEMS static shared)
        add_test(NAME RuntimeExampleInstalled_${variant} COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/sdk_gate.py"
            --source "${PROJECT_SOURCE_DIR}" --root "${ENGINE_SIM_PRESET_ROOT}"
            --out "${CMAKE_CURRENT_BINARY_DIR}/example-installed" --variant "${variant}" --examples)
        set_tests_properties(RuntimeExampleInstalled_${variant} PROPERTIES LABELS "r2-example" RUN_SERIAL TRUE TIMEOUT 600)
    endforeach ()
endif ()

if (TARGET engine-sim-runtime AND TARGET engine-sim-script-interpreter)
    add_library(engine-sim-legacy-fixture STATIC legacy_runtime.cpp)
    target_include_directories(engine-sim-legacy-fixture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(engine-sim-legacy-fixture PUBLIC engine-sim-script-interpreter PRIVATE engine-sim-runtime-backend)
    engine_sim_diagnose(engine-sim-legacy-fixture TRUE)

    set(mechanical_fixture "${CMAKE_CURRENT_BINARY_DIR}/mechanical-fixture")
    file(MAKE_DIRECTORY "${mechanical_fixture}")
    configure_file("${PROJECT_SOURCE_DIR}/test/characterization/asymmetric.mr"
        "${mechanical_fixture}/main.mr" COPYONLY)
    configure_file("${PROJECT_SOURCE_DIR}/examples/runtime/fixture/impulse.wav"
        "${mechanical_fixture}/impulse.wav" COPYONLY)
    add_executable(engine-sim-mechanical-observation-test mechanical_observation_tests.cpp)
    target_include_directories(engine-sim-mechanical-observation-test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/runtime" "${PROJECT_SOURCE_DIR}/tools/capture")
    target_link_libraries(engine-sim-mechanical-observation-test PRIVATE
        EngineSim::Runtime engine-sim-legacy-fixture gtest_main)
    engine_sim_diagnose(engine-sim-mechanical-observation-test TRUE)
    add_test(NAME ObservationIsolation COMMAND engine-sim-mechanical-observation-test
        --gtest_filter=MechanicalObservationTest.Isolation)
    add_test(NAME OutputShaftTorque COMMAND engine-sim-mechanical-observation-test
        --gtest_filter=MechanicalObservationTest.Torque:MechanicalConventionTest.OneNewtonAtOneMetreIsPositive)
    set_tests_properties(ObservationIsolation OutputShaftTorque PROPERTIES
        LABELS "r25-observation;characterization" TIMEOUT 180
        ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR};ENGINE_SIM_TEST_MECHANICAL_FIXTURE=${mechanical_fixture}")
endif ()

if (TARGET engine-sim-legacy-fixture AND TARGET engine-sim-runtime-json-private)
    add_executable(engine-sim-render-legacy-test "${PROJECT_SOURCE_DIR}/examples/runtime/render.cpp")
    target_compile_definitions(engine-sim-render-legacy-test PRIVATE ENGINE_SIM_RENDER_LEGACY_TEST)
    target_link_libraries(engine-sim-render-legacy-test PRIVATE EngineSim::Runtime engine-sim-legacy-fixture)
    target_compile_features(engine-sim-render-legacy-test PRIVATE cxx_std_17)
    set_target_properties(engine-sim-render-legacy-test PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    engine_sim_diagnose(engine-sim-render-legacy-test TRUE)

endif ()

if (TARGET engine-sim-runtime AND TARGET engine-sim-runtime-json-private)
    add_executable(engine-sim-json-runtime-test json_runtime_tests.cpp)
    target_link_libraries(engine-sim-json-runtime-test PRIVATE
        EngineSim::Runtime engine-sim-runtime-json-private engine-sim-runtime-backend)
    target_compile_features(engine-sim-json-runtime-test PRIVATE cxx_std_17)
    engine_sim_diagnose(engine-sim-json-runtime-test TRUE)

    set(acoustic_assets "${CMAKE_CURRENT_BINARY_DIR}/acoustic-fixture")
    # Supply the MIT fixture's synthetic impulse at the model's resource path.
    configure_file("${PROJECT_SOURCE_DIR}/examples/runtime/fixture/impulse.wav"
        "${acoustic_assets}/es/sound-library/new/mild_exhaust.wav" COPYONLY)
    set(acoustic_model "${PROJECT_SOURCE_DIR}/assets/engines/atg-video-1/05_honda_vtec.json")
    add_executable(engine-sim-acoustic-endpoint-test acoustic_endpoint_tests.cpp)
    target_link_libraries(engine-sim-acoustic-endpoint-test PRIVATE
        EngineSim::Runtime engine-sim-runtime-json-private engine-sim-runtime-backend gtest_main)
    engine_sim_diagnose(engine-sim-acoustic-endpoint-test TRUE)
    foreach(check IN ITEMS AcousticEndpointParity TickBoundaryOrdering)
        add_test(NAME "${check}" COMMAND engine-sim-acoustic-endpoint-test
            "--gtest_filter=AcousticEndpointTest.${check}")
        set_tests_properties("${check}" PROPERTIES LABELS r25-acoustic TIMEOUT 180
            ENVIRONMENT "ENGINE_SIM_TEST_ACOUSTIC_MODEL=${acoustic_model};ENGINE_SIM_TEST_ACOUSTIC_ASSETS=${acoustic_assets}")
    endforeach ()

    set(json_fixture "${PROJECT_SOURCE_DIR}/test/config/fixtures/schema-engine.json")
    set(impulse_fixture "${PROJECT_SOURCE_DIR}/examples/runtime/fixture/impulse.wav")
    add_test(NAME JsonTransactionalRuntime
        COMMAND engine-sim-json-runtime-test transactional "${json_fixture}" "${impulse_fixture}")
    add_test(NAME JsonResetIdentity
        COMMAND engine-sim-json-runtime-test identity "${json_fixture}" "${impulse_fixture}")
    add_test(NAME RuntimeV2Abi
        COMMAND engine-sim-json-runtime-test v2-abi "${json_fixture}" "${impulse_fixture}")
    add_test(NAME RuntimeV2Lifecycle
        COMMAND engine-sim-json-runtime-test v2-lifecycle "${json_fixture}" "${impulse_fixture}")
    set_tests_properties(JsonTransactionalRuntime JsonResetIdentity PROPERTIES LABELS r25-json-runtime TIMEOUT 90)
    set_tests_properties(RuntimeV2Abi RuntimeV2Lifecycle PROPERTIES LABELS r25-runtime-v2 TIMEOUT 90)

    add_executable(engine-sim-fleet-capacity-test fleet-capacity/main.cpp)
    target_link_libraries(engine-sim-fleet-capacity-test PRIVATE
        EngineSim::Runtime engine-sim-runtime-json-private engine-sim-runtime-backend)
    target_include_directories(engine-sim-fleet-capacity-test PRIVATE "${PROJECT_SOURCE_DIR}/modules/assembly/src")
    target_include_directories(engine-sim-fleet-capacity-test SYSTEM PRIVATE
        "${PROJECT_SOURCE_DIR}/modules/json/third_party/nlohmann")
    target_compile_features(engine-sim-fleet-capacity-test PRIVATE cxx_std_17)
    if (ENGINE_SIM_SANITIZER STREQUAL "none" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(engine-sim-fleet-capacity-test PRIVATE cost-phases/observe.cpp)
        target_include_directories(engine-sim-fleet-capacity-test PRIVATE cost-phases)
        target_compile_definitions(engine-sim-fleet-capacity-test PRIVATE ES_CAPACITY_MEASURE_ALLOCATIONS)
    endif ()
    engine_sim_diagnose(engine-sim-fleet-capacity-test TRUE)
    set(fleet_fixture "${PROJECT_SOURCE_DIR}/examples/runtime/fixture/main.json")
    add_test(NAME R3CapacityLedger COMMAND engine-sim-fleet-capacity-test ledger "${fleet_fixture}")
    add_test(NAME R3CapacityReject COMMAND engine-sim-fleet-capacity-test reject "${fleet_fixture}")
    set_tests_properties(R3CapacityLedger R3CapacityReject PROPERTIES LABELS r3-capacity TIMEOUT 90)

    add_executable(engine-sim-fleet-workspace-test fleet-workspace/main.cpp)
    target_link_libraries(engine-sim-fleet-workspace-test PRIVATE
        EngineSim::Runtime engine-sim-runtime-json-private engine-sim-runtime-backend Threads::Threads)
    target_include_directories(engine-sim-fleet-workspace-test PRIVATE "${PROJECT_SOURCE_DIR}/modules/assembly/src")
    target_compile_features(engine-sim-fleet-workspace-test PRIVATE cxx_std_17)
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        target_compile_definitions(engine-sim-fleet-workspace-test PRIVATE ES_WORKSPACE_WRAP_LOCK)
        target_link_options(engine-sim-fleet-workspace-test PRIVATE "LINKER:--wrap=pthread_mutex_lock")
    endif ()
    engine_sim_diagnose(engine-sim-fleet-workspace-test TRUE)
    add_test(NAME R3WorkspaceCold COMMAND engine-sim-fleet-workspace-test cold "${fleet_fixture}")
    add_test(NAME R3WorkspaceRollback COMMAND engine-sim-fleet-workspace-test rollback "${fleet_fixture}")
    set_tests_properties(R3WorkspaceCold R3WorkspaceRollback PROPERTIES LABELS r3-workspace TIMEOUT 120)

    add_executable(engine-sim-fleet-fault-test fleet-fault/main.cpp)
    target_link_libraries(engine-sim-fleet-fault-test PRIVATE
        EngineSim::Runtime engine-sim-runtime-json-private engine-sim-runtime-backend)
    target_compile_features(engine-sim-fleet-fault-test PRIVATE cxx_std_17)
    engine_sim_diagnose(engine-sim-fleet-fault-test TRUE)
    add_test(NAME R3FaultPrefixes COMMAND engine-sim-fleet-fault-test prefixes "${fleet_fixture}")
    add_test(NAME R3FaultNoUnwind COMMAND engine-sim-fleet-fault-test no-unwind
        "${fleet_fixture}" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(R3FaultPrefixes R3FaultNoUnwind PROPERTIES LABELS r3-fault TIMEOUT 120)

    add_executable(engine-sim-fleet-owner-test fleet-owner/main.cpp fleet-owner/consumer.c)
    target_link_libraries(engine-sim-fleet-owner-test PRIVATE
        EngineSim::Fleet EngineSim::Runtime EngineSim::Config engine-sim-runtime-backend)
    target_include_directories(engine-sim-fleet-owner-test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/fleet" "${PROJECT_SOURCE_DIR}/modules/assembly/src"
        "${PROJECT_SOURCE_DIR}/src/runtime")
    set_target_properties(engine-sim-fleet-owner-test PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if (NOT MSVC)
        target_compile_options(engine-sim-fleet-owner-test PRIVATE -pedantic-errors)
    endif ()
    engine_sim_diagnose(engine-sim-fleet-owner-test TRUE)
    add_test(NAME FleetOwnerABI COMMAND engine-sim-fleet-owner-test layout "${fleet_fixture}")
    add_test(NAME FleetCandidateCells COMMAND engine-sim-fleet-owner-test cells "${fleet_fixture}")
    set_tests_properties(FleetOwnerABI FleetCandidateCells PROPERTIES LABELS r3-fleet-owner TIMEOUT 90)

    add_executable(engine-sim-fleet-ingress-test fleet-ingress/main.cpp)
    target_link_libraries(engine-sim-fleet-ingress-test PRIVATE
        EngineSim::Fleet EngineSim::Config engine-sim-runtime-backend Threads::Threads)
    target_include_directories(engine-sim-fleet-ingress-test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/fleet" "${PROJECT_SOURCE_DIR}/modules/assembly/src"
        "${PROJECT_SOURCE_DIR}/src/runtime")
    target_compile_features(engine-sim-fleet-ingress-test PRIVATE cxx_std_17)
    engine_sim_diagnose(engine-sim-fleet-ingress-test TRUE)
    add_test(NAME FleetIngressBounds COMMAND engine-sim-fleet-ingress-test bounds "${fleet_fixture}")
    add_test(NAME FleetIngressRace COMMAND engine-sim-fleet-ingress-test race "${fleet_fixture}")
    set_tests_properties(FleetIngressBounds FleetIngressRace PROPERTIES LABELS r3-ingress TIMEOUT 90)

endif ()

if (TARGET engine-sim-runtime AND TARGET engine-sim-config-roundtrip AND ENGINE_SIM_BUILD_RUNTIME_EXAMPLES)
    add_executable(engine-sim-authoring-reload-test authoring_reload.cpp)
    target_link_libraries(engine-sim-authoring-reload-test PRIVATE
        EngineSim::Runtime engine-sim-runtime-json-private engine-sim-runtime-backend)
    engine_sim_diagnose(engine-sim-authoring-reload-test TRUE)
    add_test(NAME AuthoringHarmonicEdit COMMAND "${Python3_EXECUTABLE}" -B
        "${PROJECT_SOURCE_DIR}/test/config/authoring_edit.py" --repo "${PROJECT_SOURCE_DIR}"
        --writer "$<TARGET_FILE:engine-sim-config-roundtrip>" --reload "$<TARGET_FILE:engine-sim-authoring-reload-test>")
    add_test(NAME AuthoringCatalogParity COMMAND "${Python3_EXECUTABLE}" -B
        "${PROJECT_SOURCE_DIR}/test/config/authoring_catalog.py" verify --repo "${PROJECT_SOURCE_DIR}"
        --writer "$<TARGET_FILE:engine-sim-config-roundtrip>" --renderer "$<TARGET_FILE:engine-sim-render>")
    set_tests_properties(AuthoringHarmonicEdit PROPERTIES LABELS authoring TIMEOUT 60)
    set_tests_properties(AuthoringCatalogParity PROPERTIES LABELS authoring TIMEOUT 600)
endif ()

if (TARGET engine-sim-mechanical-observation-test AND TARGET engine-sim-runtime-json-private)
    target_link_libraries(engine-sim-mechanical-observation-test PRIVATE engine-sim-runtime-json-private EngineSim::Json)
    add_test(NAME ConvertedFixtureStructure COMMAND engine-sim-mechanical-observation-test
        --gtest_filter=MechanicalObservationTest.ConvertedFixtureStructure)
    add_test(NAME ConvertedFixtureParity COMMAND engine-sim-mechanical-observation-test
        --gtest_filter=MechanicalObservationTest.ConvertedFixtureParity)
    set_tests_properties(ConvertedFixtureStructure ConvertedFixtureParity
        PROPERTIES LABELS r25-corpus TIMEOUT 90
        ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR};ENGINE_SIM_TEST_MECHANICAL_FIXTURE=${mechanical_fixture}")
endif ()

# GNU ELF observers cover diagnostic-only linked symbols on the LP64 ABI.
if (TARGET engine-sim-runtime AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64)$"
        AND CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$" AND ENGINE_SIM_SANITIZER STREQUAL "none")
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    add_subdirectory(realtime-probe)
endif ()
