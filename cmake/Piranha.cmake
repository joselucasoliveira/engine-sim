engine_sim_locked_source(piranha_source piranha)

find_package(FLEX 2.6 REQUIRED)
find_package(BISON 3.2 REQUIRED)
set(Boost_USE_STATIC_LIBS ON)
set(Boost_USE_MULTITHREADED ON)
set(Boost_USE_STATIC_RUNTIME OFF)
find_package(Boost 1.78.0 EXACT REQUIRED COMPONENTS filesystem)

set(generated "${CMAKE_CURRENT_BINARY_DIR}/piranha")
file(MAKE_DIRECTORY "${generated}")
flex_target(lexer "${piranha_source}/flex-bison/scanner.l" "${generated}/scanner.auto.cpp"
    DEFINES_FILE "${generated}/scanner.auto.h")
bison_target(parser "${piranha_source}/flex-bison/specification.y" "${generated}/parser.auto.cpp"
    DEFINES_FILE "${generated}/parser.auto.h")
add_flex_bison_dependency(lexer parser)

add_library(piranha STATIC
    "${piranha_source}/src/assembly.cpp"
    "${piranha_source}/src/channel_node.cpp"
    "${piranha_source}/src/channel_type.cpp"
    "${piranha_source}/src/compilation_error.cpp"
    "${piranha_source}/src/compiler.cpp"
    "${piranha_source}/src/error_list.cpp"
    "${piranha_source}/src/float_conversions.cpp"
    "${piranha_source}/src/fundamental_types.cpp"
    "${piranha_source}/src/int_conversions.cpp"
    "${piranha_source}/src/ir_attribute.cpp"
    "${piranha_source}/src/ir_attribute_definition.cpp"
    "${piranha_source}/src/ir_attribute_definition_list.cpp"
    "${piranha_source}/src/ir_attribute_list.cpp"
    "${piranha_source}/src/ir_binary_operator.cpp"
    "${piranha_source}/src/ir_compilation_unit.cpp"
    "${piranha_source}/src/ir_context_tree.cpp"
    "${piranha_source}/src/ir_import_statement.cpp"
    "${piranha_source}/src/ir_node.cpp"
    "${piranha_source}/src/ir_node_definition.cpp"
    "${piranha_source}/src/ir_parser_structure.cpp"
    "${piranha_source}/src/ir_unary_operator.cpp"
    "${piranha_source}/src/ir_value.cpp"
    "${piranha_source}/src/language_rules.cpp"
    "${piranha_source}/src/memory_tracker.cpp"
    "${piranha_source}/src/node.cpp"
    "${piranha_source}/src/node_allocator.cpp"
    "${piranha_source}/src/node_container.cpp"
    "${piranha_source}/src/node_graph.cpp"
    "${piranha_source}/src/node_output.cpp"
    "${piranha_source}/src/node_program.cpp"
    "${piranha_source}/src/no_op_node.cpp"
    "${piranha_source}/src/path.cpp"
    "${piranha_source}/src/standard_allocator.cpp"
    "${piranha_source}/src/string_conversions.cpp"
    "${piranha_source}/src/vector_constructor.cpp"
    ${FLEX_lexer_OUTPUTS}
    ${BISON_parser_OUTPUTS}
)
target_include_directories(piranha
    PUBLIC "${piranha_source}/include"
    PRIVATE "${generated}" ${FLEX_INCLUDE_DIRS}
)
target_link_libraries(piranha PUBLIC Boost::filesystem)
