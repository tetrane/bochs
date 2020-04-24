#pragma once

#include <stdint.h>

// Retrieve current instruction count, that is guaranteed to match reven's trace.
uint64_t reven_icount(void);
