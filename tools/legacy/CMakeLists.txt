add_library(engine-sim-legacy-snapshot STATIC snapshot.cpp resources.cpp)
target_include_directories(engine-sim-legacy-snapshot PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(engine-sim-legacy-snapshot PUBLIC EngineSim::Config engine-sim-script-interpreter)
set_target_properties(engine-sim-legacy-snapshot PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
engine_sim_diagnose(engine-sim-legacy-snapshot TRUE)

set(converter_sources main.cpp invocation.cpp publication.cpp sha256.cpp)
add_executable(engine-sim-convert ${converter_sources} "${PROJECT_SOURCE_DIR}/src/resource_paths.cpp")
target_link_libraries(engine-sim-convert PRIVATE engine-sim-legacy-snapshot EngineSim::Json)
target_include_directories(engine-sim-convert PRIVATE "${PROJECT_SOURCE_DIR}/modules/json/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(engine-sim-convert SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/modules/json/third_party/nlohmann")
if (WIN32)
    target_link_libraries(engine-sim-convert PRIVATE shell32)
endif ()
set_target_properties(engine-sim-convert PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
engine_sim_diagnose(engine-sim-convert TRUE)

set(converter_lock "${PROJECT_SOURCE_DIR}/cmake/dependencies.lock.json")
file(SHA256 "${converter_lock}" converter_lock_hash)
file(READ "${converter_lock}" converter_lock_bytes)
string(JSON converter_parser GET "${converter_lock_bytes}" git piranha revision)
file(GLOB_RECURSE converter_identity_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
    "${PROJECT_SOURCE_DIR}/tools/legacy/*" "${PROJECT_SOURCE_DIR}/scripting/*"
    "${PROJECT_SOURCE_DIR}/modules/config/*" "${PROJECT_SOURCE_DIR}/modules/json/*"
    "${PROJECT_SOURCE_DIR}/include/*" "${PROJECT_SOURCE_DIR}/src/*"
    "${PROJECT_SOURCE_DIR}/schemas/*" "${PROJECT_SOURCE_DIR}/cmake/*")
list(APPEND converter_identity_inputs "${PROJECT_SOURCE_DIR}/CMakeLists.txt")
list(SORT converter_identity_inputs)
set(converter_identity_list "${CMAKE_CURRENT_BINARY_DIR}/build-identity-inputs.txt")
file(GENERATE OUTPUT "${converter_identity_list}" CONTENT "${converter_identity_inputs}")
set(converter_identity_header "${CMAKE_CURRENT_BINARY_DIR}/build_identity.hpp")
add_custom_command(OUTPUT "${converter_identity_header}"
    COMMAND "${CMAKE_COMMAND}"
        "-Dsource_root=${PROJECT_SOURCE_DIR}"
        "-Dsource_list=${converter_identity_list}"
        "-Didentity_header=${converter_identity_header}"
        "-Dbuild_compiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
        "-Dbuild_host=${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}"
        "-Dconverter_fp=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_RELEASE}"
        "-Dconverter_parser=${converter_parser}"
        "-Dconverter_lock_hash=${converter_lock_hash}"
        -P "${PROJECT_SOURCE_DIR}/cmake/LegacyConverterIdentity.cmake"
    DEPENDS "${converter_identity_list}" ${converter_identity_inputs}
    VERBATIM)
target_sources(engine-sim-convert PRIVATE "${converter_identity_header}")

include(GNUInstallDirs)
install(TARGETS engine-sim-convert RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT LegacyConverter)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-converter/licenses" COMPONENT LegacyConverter)
install(FILES "${PROJECT_SOURCE_DIR}/modules/json/third_party/nlohmann/LICENSE.MIT"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-converter/licenses" RENAME nlohmann-json-MIT.txt COMPONENT LegacyConverter)
install(FILES "${PROJECT_SOURCE_DIR}/dependencies/submodules/piranha/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-converter/licenses" RENAME Piranha-MIT.txt COMPONENT LegacyConverter)
install(FILES "${PROJECT_SOURCE_DIR}/dependencies/submodules/simple-2d-constraint-solver/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-converter/licenses" RENAME Solver-MIT.txt COMPONENT LegacyConverter)
install(FILES "${BOOST_ROOT}/LICENSE_1_0.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-converter/licenses" RENAME Boost.txt COMPONENT LegacyConverter)
get_target_property(converter_parser_includes piranha INCLUDE_DIRECTORIES)
foreach(directory IN LISTS converter_parser_includes)
    if (EXISTS "${directory}/FlexLexer.h")
        install(FILES "${directory}/FlexLexer.h"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/engine-sim-converter/licenses"
            RENAME FlexLexer-notice.h COMPONENT LegacyConverter)
        break()
    endif ()
endforeach ()
