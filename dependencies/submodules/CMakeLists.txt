include("${PROJECT_SOURCE_DIR}/cmake/LockedSources.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/SolverOverlay.cmake")

engine_sim_locked_source(solver_original simple-2d-constraint-solver)
engine_sim_project_solver(solver "${solver_original}")
add_library(simple-2d-constraint-solver STATIC
    ${solver}/src/rigid_body_system.cpp
    ${solver}/src/matrix.cpp
    ${solver}/src/system_state.cpp
    ${solver}/src/utilities.cpp
    ${solver}/src/ode_solver.cpp
    ${solver}/src/euler_ode_solver.cpp
    ${solver}/src/rk4_ode_solver.cpp
    ${solver}/src/constraint.cpp
    ${solver}/src/force_generator.cpp
    ${solver}/src/rigid_body.cpp
    ${solver}/src/sle_solver.cpp
    ${solver}/src/gauss_seidel_sle_solver.cpp
    ${solver}/src/fixed_position_constraint.cpp
    ${solver}/src/static_force_generator.cpp
    ${solver}/src/gravity_force_generator.cpp
    ${solver}/src/link_constraint.cpp
    ${solver}/src/gaussian_elimination_sle_solver.cpp
    ${solver}/src/spring.cpp
    ${solver}/src/line_constraint.cpp
    ${solver}/src/constant_speed_motor.cpp
    ${solver}/src/rolling_constraint.cpp
    ${solver}/src/sparse_matrix.cpp
    ${solver}/src/constant_rotation_constraint.cpp
    ${solver}/src/nsv_ode_solver.cpp
    ${solver}/src/generic_rigid_body_system.cpp
    ${solver}/src/optimized_nsv_rigid_body_system.cpp
    ${solver}/src/clutch_constraint.cpp
    ${solver}/src/conjugate_gradient_sle_solver.cpp
    ${solver}/src/rotation_friction_constraint.cpp
)
target_include_directories(simple-2d-constraint-solver
    PRIVATE "${solver}/include"
    INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/solver-overlay>")
if (NOT MSVC)
    target_compile_options(simple-2d-constraint-solver PRIVATE
        -include "${PROJECT_SOURCE_DIR}/cmake/SolverCompatibility.h")
endif ()

if (ENGINE_SIM_BUILD_LEGACY_CONVERTER OR (ENGINE_SIM_BUILD_DESKTOP AND engine_sim_windows))
    include("${PROJECT_SOURCE_DIR}/cmake/BoostFilesystem.cmake")
endif ()

if (ENGINE_SIM_BUILD_LEGACY_CONVERTER)
    include("${PROJECT_SOURCE_DIR}/cmake/Piranha.cmake")
endif ()

if (ENGINE_SIM_BUILD_DESKTOP AND engine_sim_windows)
    engine_sim_locked_source(delta delta-studio)
    add_subdirectory("${delta}" delta-studio EXCLUDE_FROM_ALL)
    if (DTV)
        engine_sim_locked_source(dtv direct-to-video)
        add_subdirectory("${dtv}" direct-to-video EXCLUDE_FROM_ALL)
    endif ()
endif ()
