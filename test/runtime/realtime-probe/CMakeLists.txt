# Keep the observer, link flags and adverse paths inside test artifacts.
add_library(engine-sim-r3-observer SHARED observe.cpp)
target_compile_features(engine-sim-r3-observer PUBLIC cxx_std_17)
target_link_libraries(engine-sim-r3-observer PRIVATE "${CMAKE_DL_LIBS}")

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/operations.inc" operations REGEX "^PROBE\\(")
set(wrapped_symbols snprintf)
foreach(operation IN LISTS operations)
    string(REGEX REPLACE "^PROBE\\(([^,]+),.*" "\\1" symbol "${operation}")
    list(APPEND wrapped_symbols "${symbol}")
endforeach()
function(r3_probe_wrap target)
    target_sources("${target}" PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/wrap.cpp")
    target_link_libraries("${target}" PRIVATE engine-sim-r3-observer Threads::Threads)
    foreach(symbol IN LISTS wrapped_symbols)
        target_link_options("${target}" PRIVATE "LINKER:--wrap=${symbol}")
    endforeach()
endfunction()

if (TARGET engine-sim-fleet-process-span-test)
    set(process_dir "${CMAKE_CURRENT_SOURCE_DIR}/../fleet-process")
    add_library(engine-sim-fleet-process-boundary SHARED "${process_dir}/boundary.cpp")
    add_executable(engine-sim-fleet-process-forbidden-test
        "${process_dir}/forbidden.cpp" "${process_dir}/boundary.cpp")
    target_compile_definitions(engine-sim-fleet-process-boundary PRIVATE ES_PROCESS_BOUNDARY=fleetProcessShared)
    target_compile_definitions(engine-sim-fleet-process-forbidden-test PRIVATE ES_PROCESS_BOUNDARY=fleetProcessDirect)
    target_link_options(engine-sim-fleet-process-boundary PRIVATE "LINKER:-Bsymbolic-functions")
    add_dependencies(engine-sim-fleet-process-forbidden-test engine-sim-fleet-process-boundary)
    target_link_libraries(engine-sim-fleet-process-forbidden-test PRIVATE "${CMAKE_DL_LIBS}")
    target_include_directories(engine-sim-fleet-process-forbidden-test PRIVATE
        "${PROJECT_SOURCE_DIR}/src/fleet" "${PROJECT_SOURCE_DIR}/modules/assembly/src"
        "${PROJECT_SOURCE_DIR}/src/runtime")
    target_link_libraries(engine-sim-fleet-process-forbidden-test PRIVATE EngineSim::Config engine-sim-runtime-backend)
    foreach(target IN ITEMS engine-sim-fleet-process-boundary engine-sim-fleet-process-forbidden-test)
        target_include_directories("${target}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries("${target}" PRIVATE EngineSim::Fleet)
        target_compile_options("${target}" PRIVATE -fno-builtin)
        target_link_options("${target}" PRIVATE "LINKER:-z,now")
        r3_probe_wrap("${target}")
        engine_sim_diagnose("${target}" TRUE)
    endforeach()
    add_test(NAME FleetProcessForbidden COMMAND "${Python3_EXECUTABLE}" -B "${process_dir}/forbidden.py"
        "$<TARGET_FILE:engine-sim-fleet-process-forbidden-test>" "$<TARGET_FILE:engine-sim-fleet-process-boundary>")
    set_tests_properties(FleetProcessForbidden PROPERTIES LABELS r3-fleet-process TIMEOUT 90)
endif ()

if (NOT ENGINE_SIM_SANITIZER STREQUAL "none")
    return()
endif ()

if (TARGET engine-sim-fleet-fault-test)
    r3_probe_wrap(engine-sim-fleet-fault-test)
    target_include_directories(engine-sim-fleet-fault-test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(engine-sim-fleet-fault-test PRIVATE ES_R3_FAULT_OBSERVER)
endif ()

add_library(engine-sim-r3-probe-kernel-static STATIC kernel.cpp nested.cpp)
add_library(engine-sim-r3-probe-nested SHARED nested.cpp)
add_library(engine-sim-r3-probe-kernel-shared SHARED kernel.cpp)
r3_probe_wrap(engine-sim-r3-probe-nested)
r3_probe_wrap(engine-sim-r3-probe-kernel-shared)
target_link_libraries(engine-sim-r3-probe-kernel-shared PRIVATE engine-sim-r3-probe-nested)
foreach(callee IN ITEMS engine-sim-r3-probe-kernel-static engine-sim-r3-probe-kernel-shared engine-sim-r3-probe-nested)
    target_compile_features("${callee}" PRIVATE cxx_std_17)
    target_compile_options("${callee}" PRIVATE -fno-builtin -fsized-deallocation)
endforeach()

foreach(linkage IN ITEMS static shared)
    add_executable("engine-sim-r3-probe-${linkage}" controls.cpp)
    target_link_libraries("engine-sim-r3-probe-${linkage}" PRIVATE
        "engine-sim-r3-probe-kernel-${linkage}" engine-sim-r3-observer Threads::Threads)
endforeach()
r3_probe_wrap(engine-sim-r3-probe-static)
add_test(NAME R3ProbeCoverage COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/verify.py"
    coverage "$<TARGET_FILE:engine-sim-r3-probe-static>" "$<TARGET_FILE:engine-sim-r3-probe-shared>"
    "$<TARGET_FILE:engine-sim-r3-probe-kernel-shared>" "$<TARGET_FILE:engine-sim-r3-probe-nested>"
    "${CMAKE_C_COMPILER}" "${CMAKE_CXX_COMPILER}" "${CMAKE_READELF}" "${CMAKE_OBJDUMP}")
set_tests_properties(R3ProbeCoverage PROPERTIES TIMEOUT 180 LABELS r3-probe)

if (NOT ENGINE_SIM_RUNTIME_SHARED)
    foreach(variant IN ITEMS observed plain)
        add_executable("engine-sim-r3-raw-${variant}" raw.cpp)
        target_link_libraries("engine-sim-r3-raw-${variant}" PRIVATE
            EngineSim::Runtime engine-sim-r3-observer Threads::Threads)
    endforeach()
    r3_probe_wrap(engine-sim-r3-raw-observed)
    add_test(NAME R3RawFirstUseObservation COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/verify.py"
        raw "$<TARGET_FILE:engine-sim-r3-raw-observed>" "$<TARGET_FILE:engine-sim-r3-raw-plain>" "${PROJECT_SOURCE_DIR}")
    set_tests_properties(R3RawFirstUseObservation PROPERTIES TIMEOUT 120 LABELS r3-probe)
endif()
