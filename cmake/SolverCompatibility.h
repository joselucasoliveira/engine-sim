#pragma once

// Supply standard declarations used by the locked solver sources.
#include <cstring>

#define __forceinline inline

// The pinned sparse matrix definitions use Matrix members in the header.
#ifdef __clang__
#include "matrix.h"
#endif
