/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file
 * @brief Application worker for Espressif's BLE OTA raw profile.
 *
 * The ring-buffer and OTA-task flow comes from Espressif's official example:
 * https://github.com/espressif/esp-iot-solution/blob/2dada1378c3a7e7d973bfc3fb030234c978febda/examples/bluetooth/ble_profiles/ble_ota/main/ble_ota_raw.c
 *
 * Local changes add an explicit lifecycle, target validation and optional XZ
 * decompression. GATT, sector framing, packet sequence, CRC, ACK and send-window
 * handling remain in the official ble_services and ble_ota_raw components.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BLE_OTA_WORKER_IDLE,
    BLE_OTA_WORKER_RECEIVING,
    BLE_OTA_WORKER_VERIFYING,
    BLE_OTA_WORKER_READY,
    BLE_OTA_WORKER_ERROR,
} ble_ota_worker_state_t;

typedef enum {
    BLE_OTA_WORKER_FAILURE_NONE,
    BLE_OTA_WORKER_FAILURE_INVALID_SIZE,
    BLE_OTA_WORKER_FAILURE_INVALID_IMAGE,
    BLE_OTA_WORKER_FAILURE_WRONG_PROJECT,
    BLE_OTA_WORKER_FAILURE_FLASH,
    BLE_OTA_WORKER_FAILURE_INTERNAL,
} ble_ota_worker_failure_t;

typedef struct {
    const char *project_name;
    uint16_t chip_id;
} ble_ota_worker_config_t;

typedef struct {
    ble_ota_worker_state_t state;
    ble_ota_worker_failure_t failure;
    uint32_t transferred;
    uint32_t total;
} ble_ota_worker_snapshot_t;

/** Initialize the worker, register raw-profile callbacks, and create its task. */
esp_err_t ble_ota_worker_init(const ble_ota_worker_config_t *config);

/** Ask the worker to stop. Returns true if the verified image is already selected. */
bool ble_ota_worker_request_stop(void);

/** Wait for the worker to stop. */
esp_err_t ble_ota_worker_stop(void);

/** Release the stopped worker's resources. */
esp_err_t ble_ota_worker_deinit(void);

/** Read the worker's application-level transfer state. */
esp_err_t ble_ota_worker_get_snapshot(ble_ota_worker_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif
