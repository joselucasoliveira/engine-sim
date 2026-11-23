#include "kernel.h"
#include "escape.hpp"

#include <cstdlib>

extern "C" int probe_nested_allocation(void) {
    void *memory = std::malloc(97);
    escape(memory);
    std::free(memory);
    return memory ? 0 : 1;
}
