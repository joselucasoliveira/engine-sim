# Match the runtime's private closure while keeping Fleet package paths separate.
function(engine_sim_install_fleet_sdk)
    set(engine_sim_sdk_version 1.0.0)
    set(engine_sim_sdk_package EngineSimFleet)
    set(engine_sim_sdk_destination "${CMAKE_INSTALL_LIBDIR}/cmake/EngineSimFleet")
    set(engine_sim_sdk_private "${CMAKE_INSTALL_LIBDIR}/engine-sim-fleet")
    set(engine_sim_sdk_built "$<TARGET_FILE:engine-sim-fleet>" "$<TARGET_LINKER_FILE:engine-sim-fleet>")
    set(engine_sim_sdk_kind STATIC)
    set(engine_sim_sdk_link_file "$<TARGET_LINKER_FILE_NAME:engine-sim-fleet>")
    set(engine_sim_sdk_runtime_file "$<TARGET_FILE_NAME:engine-sim-fleet>")
    set(config_dir "${PROJECT_BINARY_DIR}/fleet-sdk-config")
    set_property(GLOBAL PROPERTY ENGINE_SIM_SDK_INPUT_CHECKS "")

    install(SCRIPT "${config_dir}/$<CONFIG>/InstallSdkPreflight.cmake" COMPONENT FleetSDK)
    set_target_properties(engine-sim-fleet PROPERTIES VERSION "${engine_sim_sdk_version}" SOVERSION 1)
    if (MSVC)
        set_property(TARGET engine-sim-fleet PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif ()
    if (ENGINE_SIM_RUNTIME_SHARED)
        set(engine_sim_sdk_kind SHARED)
        if (WIN32)
            target_sources(engine-sim-fleet PRIVATE "${PROJECT_SOURCE_DIR}/cmake/fleet.def")
            set_target_properties(engine-sim-fleet PROPERTIES
                WINDOWS_EXPORT_ALL_SYMBOLS OFF OUTPUT_NAME engine-sim-fleet-1)
        elseif (APPLE)
            set(exports "${PROJECT_SOURCE_DIR}/cmake/fleet.macos.exports")
            target_link_options(engine-sim-fleet PRIVATE "LINKER:-exported_symbols_list,${exports}")
            set_target_properties(engine-sim-fleet PROPERTIES INSTALL_NAME_DIR "@rpath" INSTALL_RPATH "@loader_path")
            set_property(TARGET engine-sim-fleet APPEND PROPERTY LINK_DEPENDS "${exports}")
        else ()
            set(exports "${PROJECT_SOURCE_DIR}/cmake/fleet.elf.exports")
            target_link_options(engine-sim-fleet PRIVATE "LINKER:--version-script=${exports}" "LINKER:-z,now")
            set_target_properties(engine-sim-fleet PROPERTIES INSTALL_RPATH "$ORIGIN")
            set_property(TARGET engine-sim-fleet APPEND PROPERTY LINK_DEPENDS "${exports}")
        endif ()
    else ()
        foreach(target IN LISTS engine_sim_sdk_archives)
            list(APPEND engine_sim_sdk_built "$<TARGET_FILE:${target}>")
            install(FILES "$<TARGET_FILE:${target}>" DESTINATION "${engine_sim_sdk_private}" COMPONENT FleetSDK)
        endforeach ()
    endif ()
    install(TARGETS engine-sim-fleet
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT FleetSDK
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT FleetSDK NAMELINK_COMPONENT FleetSDK
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT FleetSDK)
    engine_sim_sdk_verify("${PROJECT_SOURCE_DIR}/include/engine_sim/fleet.h")
    install(FILES "${PROJECT_SOURCE_DIR}/include/engine_sim/fleet.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/engine_sim" COMPONENT FleetSDK)

    configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/EngineSimFleetConfig.cmake.in"
        "${config_dir}/EngineSimFleetConfig.cmake.gen" INSTALL_DESTINATION "${engine_sim_sdk_destination}")
    file(GENERATE OUTPUT "${config_dir}/$<CONFIG>/EngineSimFleetConfig.cmake"
        INPUT "${config_dir}/EngineSimFleetConfig.cmake.gen" NEWLINE_STYLE UNIX)
    write_basic_package_version_file("${config_dir}/EngineSimFleetConfigVersion.cmake"
        VERSION "${engine_sim_sdk_version}" COMPATIBILITY SameMajorVersion)
    engine_sim_sdk_verify("${config_dir}/EngineSimFleetConfigVersion.cmake")
    install(FILES "${config_dir}/$<CONFIG>/EngineSimFleetConfig.cmake"
        "${config_dir}/EngineSimFleetConfigVersion.cmake"
        DESTINATION "${engine_sim_sdk_destination}" COMPONENT FleetSDK)

    set(licenses
        "LICENSE|EngineSim-MIT.txt"
        "cmake/fleet-sdk-notices.md|THIRD_PARTY_NOTICES.md"
        "modules/json/third_party/nlohmann/LICENSE.MIT|nlohmann-json-MIT.txt"
        "dependencies/submodules/simple-2d-constraint-solver/LICENSE|Solver-MIT.txt")
    foreach(entry IN LISTS licenses)
        string(REPLACE "|" ";" fields "${entry}")
        list(GET fields 0 input)
        list(GET fields 1 name)
        engine_sim_sdk_verify("${PROJECT_SOURCE_DIR}/${input}")
        install(FILES "${PROJECT_SOURCE_DIR}/${input}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-fleet/licenses" RENAME "${name}" COMPONENT FleetSDK)
    endforeach ()
    get_property(engine_sim_sdk_checks GLOBAL PROPERTY ENGINE_SIM_SDK_INPUT_CHECKS)
    file(READ "${config_dir}/EngineSimFleetConfig.cmake.gen" engine_sim_sdk_config)
    if (engine_sim_sdk_config MATCHES "]===]")
        message(FATAL_ERROR "Unsupported Fleet SDK configuration delimiter")
    endif ()
    configure_file("${PROJECT_SOURCE_DIR}/cmake/InstallSdkPreflight.cmake.in"
        "${config_dir}/InstallSdkPreflight.cmake.gen" @ONLY NEWLINE_STYLE UNIX)
    file(GENERATE OUTPUT "${config_dir}/$<CONFIG>/InstallSdkPreflight.cmake"
        INPUT "${config_dir}/InstallSdkPreflight.cmake.gen" NEWLINE_STYLE UNIX)
endfunction()
engine_sim_install_fleet_sdk()

if (ENGINE_SIM_INSTALL_FLEET_EXAMPLE)
    set(manifest "${PROJECT_SOURCE_DIR}/cmake/fleet-example.json")
    set_property(GLOBAL PROPERTY ENGINE_SIM_SDK_INPUT_CHECKS "")
    engine_sim_sdk_verify("${manifest}")
    file(READ "${manifest}" examples)
    set(allowed CMakeLists.txt README.md host.c host.cpp verify.py
        fixture-heavy/LICENSE fixture-heavy/main.json fixture-heavy/impulse.wav
        fixture-single/LICENSE fixture-single/main.json fixture-single/impulse.wav)
    string(JSON count LENGTH "${examples}" files)
    math(EXPR last "${count}-1")
    install(SCRIPT "${PROJECT_BINARY_DIR}/fleet-sdk-config/ExamplesPreflight.cmake" COMPONENT FleetSDKExamples)
    foreach(i RANGE 0 ${last})
        string(JSON path GET "${examples}" files ${i} path)
        if (NOT path IN_LIST allowed)
            message(FATAL_ERROR "Unexpected or duplicate Fleet example path: ${path}")
        endif ()
        list(REMOVE_ITEM allowed "${path}")
        string(JSON source GET "${examples}" files ${i} source)
        if (IS_ABSOLUTE "${source}" OR NOT source MATCHES "^examples/(fleet|runtime/fixture)/"
                OR source MATCHES "(^|/)[.][.](/|$)")
            message(FATAL_ERROR "Fleet example source is outside its inventory: ${source}")
        endif ()
        string(JSON expected GET "${examples}" files ${i} sha256)
        set(input "${PROJECT_SOURCE_DIR}/${source}")
        engine_sim_sdk_verify("${input}")
        file(SHA256 "${input}" actual)
        if (NOT actual STREQUAL expected)
            message(FATAL_ERROR "Fleet example bytes changed: ${path}; review the explicit inventory")
        endif ()
        get_filename_component(folder "${path}" DIRECTORY)
        get_filename_component(leaf "${path}" NAME)
        install(FILES "${input}" DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-fleet/example/${folder}"
            RENAME "${leaf}" COMPONENT FleetSDKExamples)
    endforeach ()
    if (allowed)
        message(FATAL_ERROR "Fleet example inventory lacks: ${allowed}")
    endif ()
    get_property(engine_sim_sdk_checks GLOBAL PROPERTY ENGINE_SIM_SDK_INPUT_CHECKS)
    configure_file("${PROJECT_SOURCE_DIR}/cmake/InstallFleetExamplesPreflight.cmake.in"
        "${PROJECT_BINARY_DIR}/fleet-sdk-config/ExamplesPreflight.cmake" @ONLY NEWLINE_STYLE UNIX)
endif ()
