/*
 * sd_crc_compat.c - accept SD cards that refuse CMD59.
 *
 * ESP-IDF makes enabling SPI CRC a mandatory init step:
 *
 *     SDMMC_INIT_STEP(is_spi, sdmmc_init_spi_crc);   // sdmmc_init.c
 *
 * and sdmmc_init_spi_crc() returns whatever CMD59 returned. A card that answers
 * CMD59 with the illegal-command bit therefore aborts the whole mount:
 *
 *     E sdmmc_sd: sdmmc_init_spi_crc: sdmmc_send_cmd_crc_on_off returned 0x106
 *     E vfs_fat_sdmmc: sdmmc_card_init failed (0x106)
 *
 * Those cards are not faulty. The SD Physical Layer specification makes CRC16
 * on data transfers OPTIONAL in SPI mode; only native SD mode requires it. That
 * is why such a card works perfectly in a computer, in a card reader, and under
 * the Arduino SD library, none of which send CMD59 at all. Measured here on
 * KEXIN 8GB cards, which a PC reads happily and this device refused 2267 times,
 * including after being reformatted as FAT32 on a PC: the format is never even
 * looked at, because initialisation fails first.
 *
 * There is no Kconfig option to relax this in ESP-IDF v5.5.2, so the step is
 * redirected with --wrap (see main/CMakeLists.txt) rather than by patching a
 * managed component, which would be undone by the next toolchain update.
 *
 * On data integrity: a card that ACCEPTS CMD59 still gets CRC enabled, so
 * nothing is weakened for cards that work today. For a card that refuses, the
 * choice is CRC off or no card at all. The glucose CSVs carry their own CRC32
 * (sd_glucose_read in sd_logger.c), so corruption of the data that matters is
 * still detected above this layer.
 */

#include "esp_log.h"
#include "esp_err.h"
#include "sdmmc_cmd.h"
#include "esp_private/sdmmc_common.h"

static const char *TAG = "SD_CRC";

esp_err_t __real_sdmmc_init_spi_crc(sdmmc_card_t *card);

esp_err_t __wrap_sdmmc_init_spi_crc(sdmmc_card_t *card)
{
    /* Ask directly rather than through the real step, so a card that refuses
       does not also log IDF's own error line on every mount attempt. A device
       with such a card probes once a glucose cycle, for ever. */
    esp_err_t err = sdmmc_send_cmd_crc_on_off(card, true);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Card refused CMD59 (%s) — mounting with SPI CRC off. "
                  "This is allowed: CRC16 is optional in SPI mode.",
             esp_err_to_name(err));
    return ESP_OK;
}
