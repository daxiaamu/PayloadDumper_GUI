/* Local standalone tinfl configuration; no compressor or ZIP API is linked. */
#pragma once
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#if defined(_WIN64)
#define MINIZ_HAS_64BIT_REGISTERS 1
#else
#define MINIZ_HAS_64BIT_REGISTERS 0
#endif
#define MZ_FORCEINLINE inline __attribute__((always_inline))
#include "miniz_tinfl.h"
