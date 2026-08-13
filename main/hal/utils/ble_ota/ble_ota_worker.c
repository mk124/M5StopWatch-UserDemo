/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Derived from Espressif's official BLE OTA raw example at commit:
 * 2dada1378c3a7e7d973bfc3fb030234c978febda
 * https://github.com/espressif/esp-iot-solution/blob/2dada1378c3a7e7d973bfc3fb030234c978febda/examples/bluetooth/ble_profiles/ble_ota/main/ble_ota_raw.c
 *
 * The upstream partition resolution, ring-buffer receive path, OTA task, and
 * esp_ota begin/write/end/set-boot/abort flow are retained. The local lifecycle
 * lets BLE OTA exist only while the StopWatch app is open and checks the app
 * project name before selecting the update partition.
 */

#include "ble_ota_worker.h"
#include "ble_ota_xz.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_ble_ota_raw.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "xz_decompress.h"

#define OTA_TASK_STACK_SIZE 8192
#define OTA_TASK_PRIORITY   5
#define OTA_WORKER_POLL     pdMS_TO_TICKS(100)
#define OTA_RINGBUF_SIZE    (1024 * 12)

#define OTA_STOP_BIT  BIT0
#define OTA_DONE_BIT  BIT1
#define OTA_START_BIT BIT2

static const char *TAG = "ble_ota_worker";

typedef struct {
    SemaphoreHandle_t lock;
    EventGroupHandle_t events;
    RingbufHandle_t ringbuf;
    TaskHandle_t task;

    const esp_partition_t *update_partition;
    esp_ota_handle_t out_handle;

    ble_ota_worker_state_t state;
    ble_ota_worker_failure_t failure;
    uint32_t image_size;
    uint32_t total;
    uint32_t received;
    uint32_t written;
    uint32_t consumed;
    uint16_t chip_id;
    char project_name[sizeof(((esp_app_desc_t *)0)->project_name)];

    bool initialized;
    bool ota_open;
    bool compressed;
} ble_ota_worker_context_t;

static ble_ota_worker_context_t s_context;

static void context_lock(void)
{
    xSemaphoreTake(s_context.lock, portMAX_DELAY);
}

static void context_unlock(void)
{
    xSemaphoreGive(s_context.lock);
}

static bool stop_requested(void)
{
    return s_context.events != NULL && (xEventGroupGetBits(s_context.events) & OTA_STOP_BIT) != 0;
}

static void set_failure_locked(ble_ota_worker_failure_t failure)
{
    if (s_context.state == BLE_OTA_WORKER_READY || s_context.state == BLE_OTA_WORKER_ERROR) {
        return;
    }
    s_context.failure = failure;
    s_context.state   = BLE_OTA_WORKER_ERROR;
    xEventGroupSetBits(s_context.events, OTA_STOP_BIT);
}

static void set_failure(ble_ota_worker_failure_t failure, const char *message)
{
    context_lock();
    set_failure_locked(failure);
    context_unlock();
    ESP_LOGE(TAG, "%s", message);
}

static bool resolve_update_partition(void)
{
    s_context.update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_context.update_partition == NULL) {
        ESP_LOGE(TAG, "OTA partition NULL");
        return false;
    }
    return true;
}

static size_t write_to_ringbuf(const uint8_t *data, size_t size)
{
    while (!stop_requested()) {
        if (xRingbufferSend(s_context.ringbuf, (void *)data, size, OTA_WORKER_POLL) == pdTRUE) {
            return size;
        }
    }
    return 0;
}

static esp_err_t ota_begin_hook(uint32_t image_size)
{
    context_lock();
    if (stop_requested() || s_context.state != BLE_OTA_WORKER_IDLE) {
        context_unlock();
        ESP_LOGE(TAG, "OTA session already started");
        return ESP_ERR_INVALID_STATE;
    }
    if (image_size == 0 || image_size > s_context.update_partition->size) {
        set_failure_locked(BLE_OTA_WORKER_FAILURE_INVALID_SIZE);
        context_unlock();
        ESP_LOGE(TAG, "invalid application image size: %" PRIu32, image_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t transfer_size = image_size;
    s_context.compressed   = ble_ota_xz_begin(image_size, &transfer_size);
    s_context.image_size   = image_size;
    s_context.total        = transfer_size;
    s_context.received     = 0;
    s_context.written      = 0;
    s_context.consumed     = 0;
    s_context.out_handle   = 0;

    esp_err_t err = esp_ota_begin(s_context.update_partition, image_size, &s_context.out_handle);
    if (err != ESP_OK) {
        if (s_context.out_handle != 0) {
            esp_ota_abort(s_context.out_handle);
            s_context.out_handle = 0;
        }
        set_failure_locked(BLE_OTA_WORKER_FAILURE_FLASH);
        context_unlock();
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    s_context.ota_open = true;
    s_context.state    = BLE_OTA_WORKER_RECEIVING;
    xEventGroupSetBits(s_context.events, OTA_START_BIT);
    context_unlock();
    ESP_LOGI(TAG, "OTA start, application=%" PRIu32 ", transfer=%" PRIu32 " (%s)", image_size, transfer_size,
             s_context.compressed ? "XZ" : "raw");
    return ESP_OK;
}

static ble_ota_worker_failure_t validate_image_header(const uint8_t *data, size_t size)
{
    const size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (size < desc_offset + sizeof(esp_app_desc_t)) {
        return BLE_OTA_WORKER_FAILURE_INVALID_IMAGE;
    }

    esp_image_header_t image = {0};
    esp_app_desc_t app       = {0};
    memcpy(&image, data, sizeof(image));
    memcpy(&app, data + desc_offset, sizeof(app));

    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != s_context.chip_id ||
        app.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return BLE_OTA_WORKER_FAILURE_INVALID_IMAGE;
    }
    if (strncmp(app.project_name, s_context.project_name, sizeof(app.project_name)) != 0) {
        return BLE_OTA_WORKER_FAILURE_WRONG_PROJECT;
    }
    return BLE_OTA_WORKER_FAILURE_NONE;
}

static bool write_image(const uint8_t *data, size_t size)
{
    const char *error = NULL;

    context_lock();
    if (!s_context.ota_open || size > s_context.image_size - s_context.written) {
        set_failure_locked(BLE_OTA_WORKER_FAILURE_INVALID_SIZE);
        error = "decompressed data exceeds declared application size";
    } else if (s_context.written == 0) {
        const ble_ota_worker_failure_t validation = validate_image_header(data, size);
        if (validation != BLE_OTA_WORKER_FAILURE_NONE) {
            set_failure_locked(validation);
            error = "application target or project validation failed";
        }
    }
    if (error == NULL && esp_ota_write(s_context.out_handle, data, size) != ESP_OK) {
        set_failure_locked(BLE_OTA_WORKER_FAILURE_FLASH);
        error = "esp_ota_write failed";
    }
    if (error == NULL) {
        s_context.written += size;
    }
    context_unlock();

    if (error != NULL) {
        ESP_LOGE(TAG, "%s", error);
        return false;
    }
    return true;
}

static void receive_fw_callback(uint8_t *buf, uint32_t length)
{
    context_lock();
    if (s_context.state != BLE_OTA_WORKER_RECEIVING) {
        context_unlock();
        return;
    }
    if (length > s_context.total - s_context.received) {
        set_failure_locked(BLE_OTA_WORKER_FAILURE_INVALID_SIZE);
        context_unlock();
        ESP_LOGE(TAG, "received data exceeds declared transfer size");
        return;
    }
    context_unlock();

    if (write_to_ringbuf(buf, length) != length) {
        return;
    }

    context_lock();
    s_context.received += length;
    context_unlock();
}

static void abort_open_session(void)
{
    context_lock();
    if (s_context.ota_open) {
        esp_ota_abort(s_context.out_handle);
        s_context.out_handle = 0;
        s_context.ota_open   = false;
    }
    context_unlock();
}

static bool receive_raw_image(void)
{
    while (!stop_requested()) {
        size_t item_size = 0;
        uint8_t *data    = (uint8_t *)xRingbufferReceive(s_context.ringbuf, &item_size, OTA_WORKER_POLL);
        if (data == NULL) {
            continue;
        }

        const bool success = write_image(data, item_size);
        vRingbufferReturnItem(s_context.ringbuf, data);
        if (!success) {
            return false;
        }

        context_lock();
        const bool complete = s_context.written == s_context.image_size;
        context_unlock();
        if (complete) {
            return true;
        }
    }
    return false;
}

static int fill_xz(void *destination, unsigned int size)
{
    while (!stop_requested()) {
        if (s_context.consumed == s_context.total) {
            return 0;
        }

        size_t item_size       = 0;
        const size_t remaining = s_context.total - s_context.consumed;
        uint8_t *data          = (uint8_t *)xRingbufferReceiveUpTo(s_context.ringbuf, &item_size, OTA_WORKER_POLL,
                                                          size < remaining ? size : remaining);
        if (data == NULL) {
            continue;
        }

        memcpy(destination, data, item_size);
        vRingbufferReturnItem(s_context.ringbuf, data);
        s_context.consumed += item_size;
        return (int)item_size;
    }
    return -1;
}

static int flush_xz(void *data, unsigned int size)
{
    return write_image(data, size) ? (int)size : -1;
}

static void report_xz_error(const char *message)
{
    if (!stop_requested()) {
        ESP_LOGE(TAG, "%s", message);
    }
}

static bool receive_xz_image(void)
{
    int input_used   = 0;
    const int result = xz_decompress(NULL, 0, fill_xz, flush_xz, NULL, &input_used, report_xz_error);
    if (stop_requested()) {
        return false;
    }

    context_lock();
    const bool complete =
        result == 0 && input_used == (int)s_context.total && s_context.written == s_context.image_size;
    if (!complete) {
        set_failure_locked(BLE_OTA_WORKER_FAILURE_INVALID_IMAGE);
    }
    context_unlock();
    if (!complete) {
        ESP_LOGE(TAG, "XZ stream length does not match the application image");
    }
    return complete;
}

static void ota_task(void *arg)
{
    (void)arg;

    const EventBits_t bits =
        xEventGroupWaitBits(s_context.events, OTA_START_BIT | OTA_STOP_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if ((bits & OTA_STOP_BIT) != 0) {
        goto OTA_DONE;
    }

    context_lock();
    const bool compressed = s_context.compressed;
    context_unlock();
    if (!(compressed ? receive_xz_image() : receive_raw_image())) {
        abort_open_session();
        goto OTA_DONE;
    }

    context_lock();
    if (stop_requested()) {
        context_unlock();
        abort_open_session();
        goto OTA_DONE;
    }
    s_context.state      = BLE_OTA_WORKER_VERIFYING;
    esp_err_t end_err    = esp_ota_end(s_context.out_handle);
    s_context.out_handle = 0;
    s_context.ota_open   = false;
    context_unlock();
    if (end_err != ESP_OK) {
        set_failure(BLE_OTA_WORKER_FAILURE_INVALID_IMAGE, "esp_ota_end failed");
        goto OTA_DONE;
    }

    esp_app_desc_t app = {0};
    if (esp_ota_get_partition_description(s_context.update_partition, &app) != ESP_OK ||
        strncmp(app.project_name, s_context.project_name, sizeof(app.project_name)) != 0) {
        set_failure(BLE_OTA_WORKER_FAILURE_WRONG_PROJECT, "application project mismatch");
        goto OTA_DONE;
    }

    context_lock();
    if (stop_requested()) {
        context_unlock();
        goto OTA_DONE;
    }

    if (esp_ota_set_boot_partition(s_context.update_partition) != ESP_OK) {
        set_failure_locked(BLE_OTA_WORKER_FAILURE_FLASH);
        context_unlock();
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        goto OTA_DONE;
    }

    s_context.state = BLE_OTA_WORKER_READY;
    context_unlock();
    ESP_LOGI(TAG, "OTA image verified and selected");

OTA_DONE:
    xEventGroupSetBits(s_context.events, OTA_DONE_BIT);
    vTaskDelete(NULL);
}

static void cleanup_init_failure(void)
{
    esp_ble_ota_raw_set_ota_begin_cb(NULL);
    esp_ble_ota_raw_recv_fw_data_callback(NULL);
    if (s_context.ringbuf != NULL) {
        vRingbufferDelete(s_context.ringbuf);
    }
    if (s_context.events != NULL) {
        vEventGroupDelete(s_context.events);
    }
    if (s_context.lock != NULL) {
        vSemaphoreDelete(s_context.lock);
    }
    memset(&s_context, 0, sizeof(s_context));
}

esp_err_t ble_ota_worker_init(const ble_ota_worker_config_t *config)
{
    if (config == NULL || config->project_name == NULL || config->project_name[0] == '\0' ||
        strlen(config->project_name) >= sizeof(s_context.project_name) || s_context.initialized) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_context, 0, sizeof(s_context));
    s_context.state   = BLE_OTA_WORKER_IDLE;
    s_context.chip_id = config->chip_id;
    memcpy(s_context.project_name, config->project_name, strlen(config->project_name) + 1);

    s_context.lock    = xSemaphoreCreateMutex();
    s_context.events  = xEventGroupCreate();
    s_context.ringbuf = xRingbufferCreate(OTA_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (s_context.lock == NULL || s_context.events == NULL || s_context.ringbuf == NULL) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    if (!resolve_update_partition()) {
        cleanup_init_failure();
        return ESP_FAIL;
    }

    esp_ble_ota_raw_set_sector_send_window_for_ringbuf(OTA_RINGBUF_SIZE);
    esp_ble_ota_raw_set_ota_begin_cb(ota_begin_hook);
    esp_ble_ota_raw_recv_fw_data_callback(receive_fw_callback);

    if (xTaskCreate(ota_task, "ble_ota", OTA_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, &s_context.task) != pdPASS) {
        cleanup_init_failure();
        return ESP_ERR_NO_MEM;
    }
    s_context.initialized = true;
    return ESP_OK;
}

bool ble_ota_worker_request_stop(void)
{
    if (s_context.lock != NULL) {
        context_lock();
        const bool completed = s_context.state == BLE_OTA_WORKER_READY;
        if (!completed) {
            xEventGroupSetBits(s_context.events, OTA_STOP_BIT);
        }
        context_unlock();
        return completed;
    }
    return false;
}

esp_err_t ble_ota_worker_stop(void)
{
    if (!s_context.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_ota_worker_request_stop();
    if (s_context.task != NULL) {
        xEventGroupWaitBits(s_context.events, OTA_DONE_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        s_context.task = NULL;
    }
    return ESP_OK;
}

esp_err_t ble_ota_worker_deinit(void)
{
    if (!s_context.initialized || s_context.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_ble_ota_raw_set_ota_begin_cb(NULL);
    esp_ble_ota_raw_recv_fw_data_callback(NULL);
    vRingbufferDelete(s_context.ringbuf);
    vEventGroupDelete(s_context.events);
    vSemaphoreDelete(s_context.lock);
    memset(&s_context, 0, sizeof(s_context));
    return ESP_OK;
}

esp_err_t ble_ota_worker_get_snapshot(ble_ota_worker_snapshot_t *snapshot)
{
    if (!s_context.initialized || snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    context_lock();
    snapshot->state       = s_context.state;
    snapshot->failure     = s_context.failure;
    snapshot->transferred = s_context.received;
    snapshot->total       = s_context.total;
    context_unlock();
    return ESP_OK;
}
