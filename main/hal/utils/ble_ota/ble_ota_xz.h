#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Select the optional XZ stream offered by the current START command. */
bool ble_ota_xz_begin(uint32_t image_size, uint32_t *transfer_size);

#ifdef __cplusplus
}
#endif
