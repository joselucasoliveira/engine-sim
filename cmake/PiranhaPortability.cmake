# Project only the sources and generator inputs needed by the locked Piranha library.
function(engine_sim_project_piranha original projected)
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS RELATIVE "${original}"
        "${original}/include/*.h" "${original}/src/*.cpp"
        "${original}/flex-bison/*.l" "${original}/flex-bison/*.y")
    set(patched
        include/memory_management.h
        include/language_rules.h
        include/ir_value_constant.h
        src/standard_allocator.cpp
    )
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
