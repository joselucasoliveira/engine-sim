set(physics_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_library(engine-sim-physics-objects OBJECT
    "${physics_root}/src/camshaft.cpp"
    "${physics_root}/src/crankshaft.cpp"
    "${physics_root}/src/combustion_chamber.cpp"
    "${physics_root}/src/connecting_rod.cpp"
    "${physics_root}/src/cylinder_bank.cpp"
    "${physics_root}/src/cylinder_head.cpp"
    "${physics_root}/src/delay_filter.cpp"
    "${physics_root}/src/direct_throttle_linkage.cpp"
    "${physics_root}/src/dynamometer.cpp"
    "${physics_root}/src/engine.cpp"
    "${physics_root}/src/exhaust_system.cpp"
    "${physics_root}/src/fuel.cpp"
    "${physics_root}/src/function.cpp"
    "${physics_root}/src/gas_system.cpp"
    "${physics_root}/src/gaussian_filter.cpp"
    "${physics_root}/src/governor.cpp"
    "${physics_root}/src/ignition_module.cpp"
    "${physics_root}/src/impulse_response.cpp"
    "${physics_root}/src/intake.cpp"
    "${physics_root}/src/part.cpp"
    "${physics_root}/src/piston.cpp"
    "${physics_root}/src/piston_engine_simulator.cpp"
    "${physics_root}/src/simulator.cpp"
    "${physics_root}/src/standard_valvetrain.cpp"
    "${physics_root}/src/starter_motor.cpp"
    "${physics_root}/src/throttle.cpp"
    "${physics_root}/src/transmission.cpp"
    "${physics_root}/src/utilities.cpp"
    "${physics_root}/src/valvetrain.cpp"
    "${physics_root}/src/vehicle.cpp"
    "${physics_root}/src/vehicle_drag_constraint.cpp"
    "${physics_root}/src/vtec_valvetrain.cpp")
target_compile_features(engine-sim-physics-objects PRIVATE cxx_std_17)

add_library(engine-sim-physics-builders OBJECT EXCLUDE_FROM_ALL
    "${physics_root}/modules/assembly/src/mechanical.cpp"
    "${physics_root}/modules/assembly/src/definition.cpp")
target_link_libraries(engine-sim-physics-builders PRIVATE EngineSim::Config)
target_include_directories(engine-sim-physics-builders PRIVATE "${physics_root}/include")
target_compile_features(engine-sim-physics-builders PRIVATE cxx_std_17)

add_library(engine-sim-physics STATIC EXCLUDE_FROM_ALL
    $<TARGET_OBJECTS:engine-sim-physics-objects>
    $<TARGET_OBJECTS:engine-sim-physics-builders>)
add_library(EngineSim::Physics ALIAS engine-sim-physics)
target_include_directories(engine-sim-physics PUBLIC
    "${physics_root}/include" "${physics_root}/modules/assembly/src")
target_link_libraries(engine-sim-physics PUBLIC EngineSim::Config simple-2d-constraint-solver)
target_compile_features(engine-sim-physics PUBLIC cxx_std_17)
if (COMMAND engine_sim_diagnose)
    engine_sim_diagnose(engine-sim-physics-objects TRUE)
    engine_sim_diagnose(engine-sim-physics-builders TRUE)
endif ()
