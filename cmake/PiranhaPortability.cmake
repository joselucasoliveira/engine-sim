# Project only the sources and generator inputs needed by the locked Piranha library.
function(engine_sim_project_piranha original projected)
    set(preparation_sources
        "include/compiler.h=350a1c5bcd87c24b4826b630419b3ca5b2404b14f795a28ebe7b0dd4614fcbe3"
        "include/ir_compilation_unit.h=078be63376e4789dd20e487ea203f1bc09bacb51659f55134b07556adc7a13d8"
        "include/scanner.h=5290df4114456d40f07af879ac51817140c5f42efa6be1237f75f09b0b01c684"
        "src/compiler.cpp=4ccbc34e4a0c38a7d2d9f509a6dcd2f8750095205d169c8603f9fe8500b4602a"
        "src/path.cpp=c1f572d036768b1c049679fb8952c31d7ffb118d76318fce7250ac68ddb14fad"
        "src/int_conversions.cpp=d92df5411628f6c8e92bddba74bb16cce68f1a1d1ba3c459cef7efadb6d20f27"
        "flex-bison/scanner.l=70a8a180df2902b6e631e1b42c2a9dc049e1c9cc69636ed6166782f85381aa26"
    )
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS RELATIVE "${original}"
        "${original}/include/*.h" "${original}/src/*.cpp"
        "${original}/flex-bison/*.l" "${original}/flex-bison/*.y")
    set(patched
        include/memory_management.h
        include/language_rules.h
        include/ir_value_constant.h
        src/standard_allocator.cpp
        flex-bison/specification.y
        src/ir_compilation_unit.cpp
    )
    foreach(source IN LISTS preparation_sources)
        string(REPLACE "=" ";" source "${source}")
        list(GET source 0 path)
        list(APPEND patched "${path}")
    endforeach ()
    foreach(input IN LISTS inputs)
        if (NOT input IN_LIST patched)
            configure_file("${original}/${input}" "${projected}/${input}" COPYONLY)
        endif ()
    endforeach ()

    foreach(input IN LISTS patched)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}/${input}")
    endforeach ()

    engine_sim_read_piranha_source(memory "${original}/include/memory_management.h"
        bfb198429d63a4197bc8d8f2ce53588e2800516ab2314450a8a714f308ca9855)
    string(REPLACE "#include <assert.h>" "#include <cassert>\n#include <cstdint>" memory "${memory}")
    string(REPLACE "unsigned __int64 mem_size" "std::uint64_t mem_size" memory "${memory}")
    string(REPLACE "((unsigned __int64)((char *)(pointer)))" "reinterpret_cast<std::uintptr_t>(pointer)" memory "${memory}")
    file(CONFIGURE OUTPUT "${projected}/include/memory_management.h" CONTENT "${memory}" @ONLY)

    engine_sim_read_piranha_source(language "${original}/include/language_rules.h"
        69a33392ff86e55aa704c5eefb851ad724abc69a1014b7adc6550f037020cdcd)
    string(REPLACE "#define PIRANHA_LANGUAGE_RULES_H" "#define PIRANHA_LANGUAGE_RULES_H\n\n#include <type_traits>"
        language "${language}")
    string(REGEX REPLACE "        template<> std::string getLiteralBuiltinName[^\n]*\n" "" language "${language}")
    string(REPLACE [[        std::string getLiteralBuiltinName() const { return ""; }]] [=[
        std::string getLiteralBuiltinName() const {
            if constexpr (std::is_same_v<NativeType, native_bool>)
                return *m_literalRules.lookup(LiteralType::Boolean);
            else if constexpr (std::is_same_v<NativeType, native_string>)
                return *m_literalRules.lookup(LiteralType::String);
            else if constexpr (std::is_same_v<NativeType, native_int>)
                return *m_literalRules.lookup(LiteralType::Integer);
            else if constexpr (std::is_same_v<NativeType, native_float>)
                return *m_literalRules.lookup(LiteralType::Float);
            else
                return "";
        }]=] language "${language}")
    file(CONFIGURE OUTPUT "${projected}/include/language_rules.h" CONTENT "${language}" @ONLY)

    engine_sim_read_piranha_source(literals "${original}/include/ir_value_constant.h"
        722dcd8974a821ba3fc1471529bdef2c5c48c6c8cec1e0186cc1ce1d68223e9e)
    string(REPLACE
        "        template <>\n        const piranha::native_string validateData<piranha::native_string>(const piranha::native_string &data, bool recordErrors)"
        "        const piranha::native_string validateData(const piranha::native_string &data, bool recordErrors = false)"
        literals "${literals}")
    string(REPLACE "Base::template validateData<T>(" "Base::validateData(" literals "${literals}")
    file(CONFIGURE OUTPUT "${projected}/include/ir_value_constant.h" CONTENT "${literals}" @ONLY)

    engine_sim_read_piranha_source(allocator "${original}/src/standard_allocator.cpp"
        2a614ce98e0a62900327a7c71cb5f2eef0b4120fc24612f89b1eb1efbccb6f5f)
    string(REPLACE "#include <malloc.h>" "#ifdef _MSC_VER\n#include <malloc.h>\n#endif" allocator "${allocator}")
    file(CONFIGURE OUTPUT "${projected}/src/standard_allocator.cpp" CONTENT "${allocator}" @ONLY)

    engine_sim_read_piranha_source(grammar "${original}/flex-bison/specification.y"
        35315cd092a8e2df06f4b511df91190449fd8f80aab58edda348cac6e25c31df)
    # Track grammar allocations until parsing finishes. Attached nodes retain
    # their existing tree owner; discarded trees are reclaimed by the parse scope.
    string(REPLACE "%parse-param {IrCompilationUnit &driver}"
        "%parse-param {IrCompilationUnit &driver}\n%parse-param {std::vector<piranha::IrParserStructure *> &allocations}"
        grammar "${grammar}")
    set(tracking [=[
#undef yylex
#define yylex scanner.yylex

template <typename T>
T *trackParserNode(std::vector<piranha::IrParserStructure *> &nodes, T *node) {
    nodes.push_back(node);
    return node;
}
#define PARSER_TRACK(node) trackParserNode(allocations, TRACK(node))]=])
    string(REPLACE "#undef yylex\n#define yylex scanner.yylex" "${tracking}" grammar "${grammar}")
    string(REPLACE "TRACK(new Ir" "PARSER_TRACK(new Ir" grammar "${grammar}")
    string(REPLACE "| '(' error ')'                       { yyerrok; }"
        "| '(' error ')'                       { $$ = nullptr; yyerrok; }" grammar "${grammar}")
    file(CONFIGURE OUTPUT "${projected}/flex-bison/specification.y" CONTENT "${grammar}" @ONLY)

    engine_sim_read_piranha_source(unit "${original}/src/ir_compilation_unit.cpp"
        452c432d5fd9085005973430ca09ed7d011122108e5e570e41af6e66d1376d60)
    string(REPLACE "#include <sstream>" "#include <sstream>\n#include <algorithm>" unit "${unit}")
    set(scope [=[
    struct ParseAllocations {
        std::vector<IrParserStructure *> nodes;
        ~ParseAllocations() {
            // Select roots while every pointer is still alive. Freeing one
            // root recursively releases its children, including discarded RHSs.
            nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](auto *node) {
                return node->getLogicalParent() != nullptr;
            }), nodes.end());
            for (auto *node : nodes) { node->free(); delete FTRACK(node); }
        }
    } allocations;
    Parser parser(*m_scanner, *this, allocations.nodes);
    if (parser.parse() != 0) {]=])
    set(original_parser [=[    if (m_parser != nullptr) delete FTRACK(m_parser);
    try {
        m_parser = TRACK(new piranha::Parser(*m_scanner, *this));
    }
    catch (std::bad_alloc) {
        return ParseResult::Fail;
    }

    if (m_parser->parse() != 0) {]=])
    string(REPLACE "${original_parser}" "${scope}" unit "${unit}")
    engine_sim_project_piranha_preparation("${original}" "${projected}" "${unit}" ${preparation_sources})
endfunction()

function(engine_sim_project_piranha_preparation original projected unit)
    find_package(Git REQUIRED)
    set(patch "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../scripting/piranha/preparation.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch}")
    set(scratch "${projected}-preparation")
    set(paths src/ir_compilation_unit.cpp)
    string(REGEX REPLACE "[ \t]+\n" "\n" unit "${unit}")
    file(WRITE "${scratch}/src/ir_compilation_unit.cpp" "${unit}")
    foreach(source IN LISTS ARGN)
        string(REPLACE "=" ";" source "${source}")
        list(GET source 0 path)
        list(GET source 1 hash)
        engine_sim_read_piranha_source(contents "${original}/${path}" "${hash}")
        string(REGEX REPLACE "[ \t]+\n" "\n" contents "${contents}")
        file(WRITE "${scratch}/${path}" "${contents}")
        list(APPEND paths "${path}")
    endforeach ()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c core.autocrlf=false apply
        --no-index --ignore-space-change --whitespace=error -- "${patch}"
        WORKING_DIRECTORY "${scratch}"
        COMMAND_ERROR_IS_FATAL ANY)
    foreach(path IN LISTS paths)
        configure_file("${scratch}/${path}" "${projected}/${path}" COPYONLY)
    endforeach ()
endfunction()

function(engine_sim_read_piranha_source output path expected)
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(SHA256 actual "${contents}")
    if (NOT actual STREQUAL expected)
        message(FATAL_ERROR "Cannot project ${path}: SHA-256 ${actual}, expected ${expected}")
    endif ()
    set("${output}" "${contents}" PARENT_SCOPE)
endfunction()
