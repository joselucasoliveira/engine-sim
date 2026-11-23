include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

foreach(directory IN ITEMS CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_DATADIR)
    if (IS_ABSOLUTE "${${directory}}" OR "${${directory}}" MATCHES "(^|/)[.][.](/|$)|[;\\\\]")
        message(FATAL_ERROR "The relocatable Config SDK requires a relative ${directory} within its prefix")
    endif ()
endforeach ()
set(engine_sim_config_destination "${CMAKE_INSTALL_LIBDIR}/cmake/EngineSimConfig")
# Use the same archive locations and target identities as the static RuntimeSDK.
set(engine_sim_config_private "${CMAKE_INSTALL_LIBDIR}/engine-sim-runtime")
set(engine_sim_config_targets engine-sim-config)
set(engine_sim_config_has_json FALSE)
if (TARGET engine-sim-json AND ENGINE_SIM_CONFIG_JSON)
    set(engine_sim_config_has_json TRUE)
    list(APPEND engine_sim_config_targets engine-sim-json)
endif ()
foreach(target IN LISTS engine_sim_config_targets)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
    if (MSVC)
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif ()
    install(TARGETS ${target} ARCHIVE DESTINATION "${engine_sim_config_private}" COMPONENT ConfigSDK)
endforeach ()

install(FILES "${PROJECT_SOURCE_DIR}/modules/config/include/engine_sim/config-v1.hpp"
    "${PROJECT_SOURCE_DIR}/modules/config/include/engine_sim/config-validation.hpp"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/engine_sim" COMPONENT ConfigSDK)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-config/licenses"
    RENAME EngineSim-MIT.txt COMPONENT ConfigSDK)
if (engine_sim_config_has_json)
    install(FILES "${PROJECT_SOURCE_DIR}/modules/json/include/engine_sim/config-json.hpp"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/engine_sim" COMPONENT ConfigSDK)
    install(FILES "${PROJECT_SOURCE_DIR}/modules/json/third_party/nlohmann/LICENSE.MIT"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-config/licenses" RENAME nlohmann-json-MIT.txt COMPONENT ConfigSDK)
    install(FILES "${PROJECT_SOURCE_DIR}/schemas/engine-config-v1.schema.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-config/schema" COMPONENT ConfigSDK)
endif ()

configure_package_config_file("${PROJECT_SOURCE_DIR}/cmake/EngineSimConfigConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/EngineSimConfigConfig.cmake" INSTALL_DESTINATION "${engine_sim_config_destination}")
write_basic_package_version_file("${PROJECT_BINARY_DIR}/EngineSimConfigConfigVersion.cmake"
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(FILES "${PROJECT_BINARY_DIR}/EngineSimConfigConfig.cmake" "${PROJECT_BINARY_DIR}/EngineSimConfigConfigVersion.cmake"
    DESTINATION "${engine_sim_config_destination}" COMPONENT ConfigSDK)
