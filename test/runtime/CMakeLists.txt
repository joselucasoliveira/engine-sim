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
