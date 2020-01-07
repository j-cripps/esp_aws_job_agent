/**
 * @file    ma_jobOTA.c
 *
 * @brief   Contains functions relating to the OTA (over-the-air) update feature via HTTPS
 *
 * @note    Based on the 'advanced_https_ota_example' in ESP-IDF release/v4.0
 *
 * @author  Jack Cripps
 * @date    25/10/2019
 */

#include "ma_jobOTA.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"


/* -------------------------------------------------------------------------- */
/**
 * @brief   Size of OTA data write buffer in bytes
 */
#define BUFFSIZE 1024


/* -------------------------------------------------------------------------- */


static const char *TAG = "ma_JobOTA";   /**< ESP-IDF Logging file name */

static char otaWriteData[BUFFSIZE + 1] = { 0 };     /**< OTA data write buffer ready to write to the flash */


/* Private Function Prototypes
 * -------------------------------------------------------------------------- */
/**
 * @brief   Point of no return function for when code hits unrecoverable error
 */
static void __attribute__((noreturn)) task_fatal_error();

/**
 * @brief   Called to close http connection and free any resources used by a http client after it has finished all work
 *          or encountered an error
 *
 * @param[in]   client  The esp http client handle to close and clean up
 */
static void http_cleanup(esp_http_client_handle_t client);


/* Private Function Definitions
 * -------------------------------------------------------------------------- */
static void __attribute__((noreturn)) task_fatal_error()
{
    ESP_LOGE(TAG, "Exiting task due to fatal error");
    (void)vTaskDelete(NULL);

    while (1)
    {
        ;
    }
}


static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}


/* Function Definitions
 * -------------------------------------------------------------------------- */
esp_err_t httpsOtaUpdate(const char* appVersion, const char* url, const uint8_t* serverCertPemStart)
{
    esp_err_t err;

    esp_ota_handle_t updateHandle = 0;
    const esp_partition_t *updatePartition = NULL;

    ESP_LOGI(TAG, "Starting https OTA update");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running)
    {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    esp_http_client_config_t config = {
            .url = url,
            .cert_pem = (char *)serverCertPemStart
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        return MA_OTA_HTTP_ERR;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return MA_OTA_HTTP_ERR;
    }

    esp_http_client_fetch_headers(client);

    updatePartition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             updatePartition->subtype, updatePartition->address);
    assert(updatePartition != NULL);

    int32_t binaryFileLength = 0;

    bool imageHeaderWasChecked = false;
    while (1)
    {
        int data_read = esp_http_client_read(client, otaWriteData, BUFFSIZE);

        if (data_read < 0)
        {
            ESP_LOGE(TAG, "Error: SSL data read error");
            http_cleanup(client);
            return MA_OTA_HTTP_ERR;
        }
        else if (data_read > 0)
        {
            if (imageHeaderWasChecked == false)
            {
                esp_app_desc_t new_app_info;
                if (data_read > sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
                {
                    /* check current version with downloading version */
                    memcpy(&new_app_info, &otaWriteData[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)],
                          sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New firmware version: %s", new_app_info.version);

                    if (strncmp(appVersion, new_app_info.version, strlen(appVersion)) != 0)
                    {
                        ESP_LOGW(TAG, "Job document error, listed version: %s\tbinary version: %s", appVersion, new_app_info.version);
                        http_cleanup(client);
                        return MA_OTA_JOB_ERR;
                    }

                    esp_app_desc_t running_app_info;
                    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                    }

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK)
                    {
                        ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                    }

                    /* check current version with last invalid partition */
                    if (last_invalid_app != NULL)
                    {
                        if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0)
                        {
                            ESP_LOGW(TAG, "New version is the same as invalid version.");
                            ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.",
                                    invalid_app_info.version);
                            ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                            http_cleanup(client);
                            return MA_OTA_INV_FW;
                        }
                    }

                    if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0)
                    {
                        ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
                        http_cleanup(client);
                        return MA_OTA_DUP_FW;
                    }

                    imageHeaderWasChecked = true;

                    err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &updateHandle);
                    if (err != ESP_OK)
                    {
                        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        http_cleanup(client);
                        return MA_OTA_ERR;
                    }
                    ESP_LOGI(TAG, "esp_ota_begin succeeded");
                }
                else
                {
                    ESP_LOGE(TAG, "received package is not fit len");
                    http_cleanup(client);
                    return MA_OTA_ERR;
                }
            }

            err = esp_ota_write( updateHandle, (const void *)otaWriteData, data_read);
            if (err != ESP_OK)
            {
                http_cleanup(client);
                return MA_OTA_ERR;
            }
            binaryFileLength += data_read;
            ESP_LOGD(TAG, "Written image length %d", binaryFileLength);
        }
        else if (data_read == 0)
        {
            ESP_LOGI(TAG, "Connection closed,all data received");
            break;
        }
    }
    ESP_LOGI(TAG, "Total Write binary data length : %d", binaryFileLength);

    if (esp_ota_end(updateHandle) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_end failed!");
        http_cleanup(client);
        return MA_OTA_ERR;
    }

    err = esp_ota_set_boot_partition(updatePartition);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        return MA_OTA_ERR;
    }

    http_cleanup(client);
    return MA_OTA_SUCCESS;
}
