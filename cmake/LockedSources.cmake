find_package(Git REQUIRED)
file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies.lock.json" engine_sim_dependency_lock)

function(engine_sim_locked_source result name)
    string(JSON expected GET "${engine_sim_dependency_lock}" git "${name}" revision)
    set(source "${PROJECT_SOURCE_DIR}/dependencies/submodules/${name}")
    set(init "git submodule update --init -- dependencies/submodules/${name}")
    if (NOT EXISTS "${source}/.git")
        message(FATAL_ERROR "Missing ${name} at ${expected}. Run ${init}.")
    endif ()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source}" rev-parse HEAD
        RESULT_VARIABLE git_status OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT git_status EQUAL 0 OR NOT actual STREQUAL expected)
        message(FATAL_ERROR "${name} is at ${actual}, expected ${expected}. Run ${init}.")
    endif ()
    execute_process(COMMAND "${GIT_EXECUTABLE}" --no-optional-locks -C "${source}"
        status --porcelain --untracked-files=all
        RESULT_VARIABLE git_status OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT git_status EQUAL 0 OR NOT changes STREQUAL "")
        message(FATAL_ERROR "${name} at ${expected} has local changes or could not be inspected:\n${changes}")
    endif ()
    set("${result}" "${source}" PARENT_SCOPE)
endfunction()
