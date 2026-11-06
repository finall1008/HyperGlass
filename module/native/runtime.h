#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
struct hg_patch {
 uintptr_t address;
 uint32_t size;
 uint8_t group;
 uint8_t bytes[256], original[256];
};
int hg_apply_batch(const struct hg_patch *patches, size_t count);
unsigned hg_runtime_tick(const char *phase);
bool hg_runtime_complete(void);
void hg_install_resources(void);
int hg_install_native_hook(void *target,void *replacement,void **original);
int hg_remove_native_hook(void *target);
#ifdef __cplusplus
}
#endif
