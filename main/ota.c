/*
 * Copyright (c) The Libre Solar Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef UNIT_TEST

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "cJSON.h"
#include "errno.h"
#include "driver/gpio.h"

static const char *TAG = "esp_ota";

#define DIAGNOSTIC_PIN 4
#define BUFFSIZE 1024
static char ota_write_data[BUFFSIZE + 1] = { 0 };

static bool diagnostic(void)
{
    gpio_config_t io_conf;
    io_conf.intr_type    = GPIO_INTR_DISABLE;
    io_conf.mode         = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << DIAGNOSTIC_PIN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Diagnostics (5 sec)...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);

    bool diagnostic_is_ok = gpio_get_level(DIAGNOSTIC_PIN);

    gpio_reset_pin(DIAGNOSTIC_PIN);
    return diagnostic_is_ok;
}

void esp_ota_check_image()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // run diagnostic function ...
            bool diagnostic_is_ok = diagnostic();
            if (diagnostic_is_ok) {
                ESP_LOGI(TAG, "Diagnostics completed successfully! Continuing execution ...");
                esp_ota_mark_app_valid_cancel_rollback();
            }
            else {
                ESP_LOGE(TAG, "Diagnostics failed! Start rollback to the previous version ...");
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }
}

esp_err_t esp_ota_handler(httpd_req_t *req, cJSON* res)
{
    esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

    if (configured != running) {
        ESP_LOGW(TAG, "Configured boot partition at offset 0x%08x, is running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This happens if either the ota data or boot image become corrupted.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);

    int binary_file_length = req->content_len;
    int data_read = 0;
    int total_received = 0;

    int len_img_head = sizeof(esp_image_header_t);
    int len_img_seg_head = sizeof(esp_image_segment_header_t);
    /* deal with all receive packets */
    bool image_header_was_checked = false;
    while (binary_file_length - total_received > 0) {
        data_read = httpd_req_recv(req, ota_write_data, BUFFSIZE);
        total_received += data_read;
        if (data_read < 0) {
            ESP_LOGE(TAG, "Could not receive image");
            cJSON_AddStringToObject(res, "error", "Could not receive image");
            return ESP_FAIL;
        }
        else if (data_read > 0) {
            if (image_header_was_checked == false) {
                esp_app_desc_t new_app_info;
                int len = len_img_head
                        + len_img_seg_head
                        + sizeof(esp_app_desc_t);
                if (data_read >  len) {
                    // check current version with downloading
                    memcpy(&new_app_info,
                        &ota_write_data[len_img_head + len_img_seg_head],
                        sizeof(esp_app_desc_t));
                    ESP_LOGI(TAG, "New fw version: %s", new_app_info.version);

                    esp_app_desc_t running_app_info;
                    int resp = esp_ota_get_partition_description(running, &running_app_info);
                    if (resp == ESP_OK) {
                        ESP_LOGI(TAG, "Running fw version: %s", running_app_info.version);
                    }

                    const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                    esp_app_desc_t invalid_app_info;
                    resp = esp_ota_get_partition_description(last_invalid_app, &invalid_app_info);
                    if (resp == ESP_OK) {
                        ESP_LOGI(TAG, "Last invalid fw version: %s", invalid_app_info.version);
                    }

                    // check current version with last invalid partition
                    if (last_invalid_app != NULL) {
                        int bytes_coppied = memcmp(invalid_app_info.version,
                                                    new_app_info.version,
                                                    sizeof(new_app_info.version));
                        if (bytes_coppied == 0) {
                            ESP_LOGW(TAG, "New version is the same as invalid version.");
                            ESP_LOGW(TAG, "Tried to launch the fw with %s version, but it failed.",
                                    invalid_app_info.version);
                            ESP_LOGW(TAG, "The fw has been rolled back to the last version.");
                            cJSON_AddStringToObject(res,
                                "error",
                                "New version is invalid, fw has been rolled back to last version");
                            return ESP_FAIL;
                        }
                    }
                    image_header_was_checked = true;
                    err = esp_ota_begin(update_partition, binary_file_length, &update_handle);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                        esp_ota_abort(update_handle);
                        cJSON_AddStringToObject(res, "error", "OTA begin failed");
                        return ESP_FAIL;
                    }
                    ESP_LOGI(TAG, "esp_ota_begin succeeded");
                }
                else {
                    ESP_LOGE(TAG, "received package does not fit len");
                    esp_ota_abort(update_handle);
                    cJSON_AddStringToObject(res, "error", "received package does not fit len");
                    return ESP_FAIL;
                }
            }
            err = esp_ota_write(update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                esp_ota_abort(update_handle);
                cJSON_AddStringToObject(res, "error", "Unable to write chunk to flash");
                return ESP_FAIL;
            }
            ESP_LOGD(TAG, "Written image length %d", total_received);
        }
        else if (data_read == 0) {
            if (errno == ECONNRESET || errno == ENOTCONN) {
                ESP_LOGE(TAG, "Connection closed, errno = %d", errno);
                break;
            }
            if (total_received == binary_file_length) {
                ESP_LOGI(TAG, "Connection closed");
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Total image size received: %d bytes", total_received);
    if (total_received != binary_file_length) {
        ESP_LOGI(TAG, "Error in receiving complete file");
        esp_ota_abort(update_handle);
        cJSON_AddStringToObject(res, "error", "Unable to write chunk to flash");
        return ESP_FAIL;
    }
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            cJSON_AddStringToObject(res, "error", "Image validation failed, image is corrupted");
        }
        else {
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
            cJSON_AddStringToObject(res, "error", "esp_ota_end failed");
        }
        return ESP_FAIL;
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        cJSON_AddStringToObject(res, "error", "esp_ota_set_boot_partition failed");
    }

    return ESP_OK;
}

void esp_ota_reset_device()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI("reset_task", "Prepare to restart system!");
    esp_restart();
}

#endif
