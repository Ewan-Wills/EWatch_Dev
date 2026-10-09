#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint32_t esp_random(void);   // deterministic PRNG on the host (seeded by --seed)
void esp_fill_random(void *buf, size_t len);
#ifdef __cplusplus
}
#endif
