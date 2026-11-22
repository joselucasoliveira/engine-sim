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

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-diagnostics-test diagnostics_tests.cpp)
    target_link_libraries(engine-sim-diagnostics-test PRIVATE EngineSim::Runtime engine-sim-script-interpreter gtest_main)
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
    add_test(NAME RuntimeCConsumer COMMAND engine-sim-runtime-c-test "${fixture}/main.mr" "${fixture}/library" "${fixture}")
    set_tests_properties(RuntimeCConsumer PROPERTIES LABELS "r2-runtime" TIMEOUT 180)
    add_executable(engine-sim-runtime-test runtime_tests.cpp)
    target_link_libraries(engine-sim-runtime-test PRIVATE EngineSim::Runtime engine-sim-script-interpreter gtest_main)
    target_include_directories(engine-sim-runtime-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-runtime-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime-working")
    gtest_discover_tests(engine-sim-runtime-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-runtime" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-runtime-lifecycle-test lifecycle_tests.cpp)
    target_link_libraries(engine-sim-runtime-lifecycle-test PRIVATE EngineSim::Runtime engine-sim-script-interpreter gtest_main)
    target_include_directories(engine-sim-runtime-lifecycle-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-runtime-lifecycle-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-working")
    gtest_discover_tests(engine-sim-runtime-lifecycle-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lifecycle-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-lifecycle" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-rate-test rate_tests.cpp)
    target_link_libraries(engine-sim-rate-test PRIVATE EngineSim::Runtime engine-sim-script-interpreter gtest_main)
    target_include_directories(engine-sim-rate-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-rate-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rate-working")
    gtest_discover_tests(engine-sim-rate-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rate-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-rate" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-runtime-load-test load_tests.cpp)
    target_link_libraries(engine-sim-runtime-load-test PRIVATE EngineSim::Runtime engine-sim-script-interpreter gtest_main)
    target_include_directories(engine-sim-runtime-load-test PRIVATE "${PROJECT_SOURCE_DIR}/src/runtime")
    engine_sim_diagnose(engine-sim-runtime-load-test TRUE)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/load-working")
    gtest_discover_tests(engine-sim-runtime-load-test WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/load-working"
        DISCOVERY_TIMEOUT 30 PROPERTIES LABELS "r2-load" TIMEOUT 180 ENVIRONMENT "ENGINE_SIM_TEST_SOURCE=${PROJECT_SOURCE_DIR}")
endif ()

if (TARGET engine-sim-runtime)
    add_executable(engine-sim-drive-test drive_tests.cpp)
    target_link_libraries(engine-sim-drive-test PRIVATE EngineSim::Runtime engine-sim-script-interpreter gtest_main)
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
    # Timings require an optimized, unsanitized static implementation. The probe
    # is a different executable so allocation and stage clocks cannot affect it.
    if (NOT ENGINE_SIM_RUNTIME_SHARED AND ENGINE_SIM_SANITIZER STREQUAL "none")
        foreach(variant IN ITEMS bench probe)
            add_executable(engine-sim-runtime-${variant} benchmark.cpp)
            target_link_libraries(engine-sim-runtime-${variant} PRIVATE EngineSim::Runtime engine-sim-script-interpreter)
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
