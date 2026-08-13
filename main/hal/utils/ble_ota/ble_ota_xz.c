#include "ble_ota_xz.h"

#include <stddef.h>
#include <string.h>

#include "esp_ble_ota_svc.h"

#define COMMAND_START 0x0001
#define COMMAND_ACK   0x0003
#define ACK_ACCEPT    0x0000
#define PACKET_SIZE   20
#define CRC_OFFSET    18

// START: command[0..1], image size[2..5], reserved[6..9], magic[10..13], XZ size[14..17], CRC[18..19].
// ACK: command/status[0..5], window[6], reserved[7..12], magic[13..16], XZ selected[17], CRC[18..19].
static const uint8_t XZ_MAGIC[] = {'M', '5', 'Z', '1'};

static uint32_t s_xz_size;

static uint16_t get_u16_le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t get_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static uint16_t crc16(const uint8_t *data, size_t size)
{
    uint16_t crc = 0;
    while (size-- > 0) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0 ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

void ble_ota_xz_recv_cmd_data(const uint8_t *data, uint16_t len)
{
    if (data != NULL && len == PACKET_SIZE && get_u16_le(data) == COMMAND_START) {
        const uint32_t image_size = get_u32_le(data + 2);
        const uint32_t xz_size    = get_u32_le(data + 14);
        s_xz_size = get_u32_le(data + 6) == 0 && memcmp(data + 10, XZ_MAGIC, sizeof(XZ_MAGIC)) == 0 && xz_size > 0 &&
                            xz_size < image_size
                        ? xz_size
                        : 0;
    }
    esp_ble_ota_recv_cmd_data(data, len);
}

esp_err_t ble_ota_xz_notify_command_raw(const uint8_t *data, uint16_t len)
{
    const bool start_ack =
        data != NULL && len == PACKET_SIZE && get_u16_le(data) == COMMAND_ACK && get_u16_le(data + 2) == COMMAND_START;
    if (!start_ack || get_u16_le(data + 4) != ACK_ACCEPT || s_xz_size == 0) {
        const esp_err_t result = esp_ble_ota_notify_command_raw(data, len);
        if (start_ack) {
            s_xz_size = 0;
        }
        return result;
    }

    uint8_t ack[PACKET_SIZE];
    memcpy(ack, data, sizeof(ack));
    memcpy(ack + 13, XZ_MAGIC, sizeof(XZ_MAGIC));
    ack[17]            = 1;
    const uint16_t crc = crc16(ack, CRC_OFFSET);
    ack[18]            = (uint8_t)crc;
    ack[19]            = (uint8_t)(crc >> 8);

    s_xz_size = 0;
    return esp_ble_ota_notify_command_raw(ack, sizeof(ack));
}

bool ble_ota_xz_begin(uint32_t image_size, uint32_t *transfer_size)
{
    const bool selected = transfer_size != NULL && s_xz_size > 0 && s_xz_size < image_size;
    if (selected) {
        *transfer_size = s_xz_size;
    }
    return selected;
}
