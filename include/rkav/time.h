#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// CLOCK_MONOTONIC 微秒
uint64_t rkav_now_monotonic_us(void);

#ifdef __cplusplus
}
#endif
