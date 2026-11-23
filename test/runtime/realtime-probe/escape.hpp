#ifndef ENGINE_SIM_TEST_REALTIME_ESCAPE_HPP
#define ENGINE_SIM_TEST_REALTIME_ESCAPE_HPP

inline void escape(const void *pointer) {
    asm volatile("" : : "g"(pointer) : "memory");
}
#endif
