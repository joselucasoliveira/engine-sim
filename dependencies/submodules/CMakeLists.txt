include("${PROJECT_SOURCE_DIR}/cmake/LockedSources.cmake")

engine_sim_locked_source(solver simple-2d-constraint-solver)
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
target_include_directories(simple-2d-constraint-solver PUBLIC "${solver}/include")

engine_sim_locked_source(csv csv-io)
add_library(csv-io STATIC "${csv}/src/csv_data.cpp" "${csv}/src/information.cpp")
target_compile_definitions(csv-io PRIVATE PROJECT_NAMESPACE=atg_csv)
target_include_directories(csv-io PUBLIC "${csv}/include")

if (PIRANHA_ENABLED)
    include("${PROJECT_SOURCE_DIR}/cmake/Piranha.cmake")
endif ()

if (ENGINE_SIM_BUILD_DESKTOP)
    engine_sim_locked_source(delta delta-studio)
    add_subdirectory("${delta}" delta-studio EXCLUDE_FROM_ALL)
    if (DTV)
        engine_sim_locked_source(dtv direct-to-video)
        add_subdirectory("${dtv}" direct-to-video EXCLUDE_FROM_ALL)
    endif ()
endif ()
