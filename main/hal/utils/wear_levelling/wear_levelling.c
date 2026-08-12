#include "wear_levelling.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"

static const char *TAG        = "wear_levelling";
static const char base_path[] = "/spiflash";

void wear_levelling_init(void)
{
    ESP_LOGI(TAG, "Mounting FAT filesystem");
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files              = 4,
        .format_if_mount_failed = true,
        .allocation_unit_size   = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat            = false,
    };
    wl_handle_t wl_handle;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(base_path, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Done");
}

const char *wear_levelling_get_base_path(void)
{
    return base_path;
}
