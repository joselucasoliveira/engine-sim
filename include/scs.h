#ifndef ATG_ENGINE_SIM_SCS_H
#define ATG_ENGINE_SIM_SCS_H

// The locked solver uses the MSVC spelling for inline member functions.
#if !defined(_MSC_VER)
#define __forceinline inline
#endif
#include <engine_sim_solver/include/scs.h>
#if !defined(_MSC_VER)
#undef __forceinline
#endif

#endif /* ATG_ENGINE_SIM_SCS_H */
