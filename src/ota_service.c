#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "ota_service.h"

#define TAG "ota_service"

#define OTA_BUFFER_SIZE 1024
#define OTA_MAX_SIZE (4 * 1024 * 1024) // 4MB Max size based on partition table

static ota_service_config_t s_config = {0};
static ota_service_update_info_t s_update_info = {0};
static bool s_initialized = false;

esp_err_t ota_service_init(const ota_service_config_t *config)
{
    if (!config || !config->check_url) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    s_initialized = true;
    return ESP_OK;
}

static esp_err_t _ota_check_http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                typedef struct {
                    char *buffer;
                    int len;
                    int index;
                } ota_json_buffer_t;
                
                ota_json_buffer_t *buf = (ota_json_buffer_t *)evt->user_data;
                if (buf->index + evt->data_len < buf->len) {
                    memcpy(buf->buffer + buf->index, evt->data, evt->data_len);
                    buf->index += evt->data_len;
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

typedef struct {
    char *buffer;
    int len;
    int index;
} ota_json_buffer_t;

esp_err_t ota_service_check_update(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    ota_service_event_data_t event_data;
    event_data.event = OTA_SERVICE_EVENT_START_CHECK;
    if (s_config.callback) s_config.callback(&event_data);

    ota_json_buffer_t json_buf = {0};
    json_buf.len = 2048; // Max JSON size
    json_buf.buffer = malloc(json_buf.len);
    if (!json_buf.buffer) return ESP_ERR_NO_MEM;
    json_buf.index = 0;

    esp_http_client_config_t config = {
        .url = s_config.check_url,
        .event_handler = _ota_check_http_event_handler,
        .user_data = &json_buf,
        .timeout_ms = 5000,
        .buffer_size = 2048,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(json_buf.buffer);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            json_buf.buffer[json_buf.index] = 0; // Null terminate
            ESP_LOGI(TAG, "JSON received: %s", json_buf.buffer);
            
            cJSON *json = cJSON_Parse(json_buf.buffer);
            if (json) {
                cJSON *version = cJSON_GetObjectItem(json, "version");
                cJSON *url = cJSON_GetObjectItem(json, "url");
                cJSON *desc = cJSON_GetObjectItem(json, "description");
                cJSON *mandatory = cJSON_GetObjectItem(json, "mandatory");
                cJSON *size = cJSON_GetObjectItem(json, "size");

                if (version && version->valuestring && url && url->valuestring) {
                    strncpy(s_update_info.version, version->valuestring, sizeof(s_update_info.version) - 1);
                    strncpy(s_update_info.url, url->valuestring, sizeof(s_update_info.url) - 1);
                    if (desc && desc->valuestring) strncpy(s_update_info.description, desc->valuestring, sizeof(s_update_info.description) - 1);
                    if (mandatory) s_update_info.mandatory = cJSON_IsTrue(mandatory);
                    if (size) s_update_info.total_size = size->valueint;

                    if (s_update_info.total_size > OTA_MAX_SIZE) {
                         ESP_LOGE(TAG, "Firmware size %lu exceeds partition limit %d", s_update_info.total_size, OTA_MAX_SIZE);
                         event_data.event = OTA_SERVICE_EVENT_ERROR;
                         event_data.data.error_msg = "Firmware size exceeds limit";
                         if (s_config.callback) s_config.callback(&event_data);
                    } else {
                        const esp_app_desc_t *app_desc = esp_app_get_description();
                        if (strcmp(app_desc->version, s_update_info.version) != 0) {
                            event_data.event = OTA_SERVICE_EVENT_UPDATE_AVAILABLE;
                            event_data.data.update_info = s_update_info;
                            if (s_config.callback) s_config.callback(&event_data);
                        } else {
                            event_data.event = OTA_SERVICE_EVENT_NO_UPDATE;
                            if (s_config.callback) s_config.callback(&event_data);
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "JSON missing fields");
                    err = ESP_FAIL;
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "JSON Parse error");
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "HTTP Status Code: %d", status_code);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP Request failed: %s", esp_err_to_name(err));
    }

    free(json_buf.buffer);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t ota_service_start_update(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (strlen(s_update_info.url) == 0) {
        ESP_LOGE(TAG, "No update URL found. Run check_update first.");
        return ESP_ERR_INVALID_STATE;
    }

    ota_service_event_data_t event_data;
    event_data.event = OTA_SERVICE_EVENT_START_DOWNLOAD;
    if (s_config.callback) s_config.callback(&event_data);

    esp_http_client_config_t config = {
        .url = s_update_info.url,
        .timeout_ms = 5000,
        .keep_alive_enable = true,
#ifdef CONFIG_OTA_ALLOW_HTTP
        .cert_pem = NULL,
        .skip_cert_common_name_check = true,
#endif
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .http_client_init_cb = NULL,
    };

    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA Begin failed");
        return err;
    }

    int64_t start_time = esp_timer_get_time();
    int64_t last_time = start_time;

    while (1) {
        err = esp_https_ota_perform(https_ota_handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        // Calculate progress
        int read_len = esp_https_ota_get_image_len_read(https_ota_handle);
        int total_len = esp_https_ota_get_image_size(https_ota_handle);
        
        int64_t current_time = esp_timer_get_time();
        if (current_time - last_time > 1000000) { // Update every 1 second
            float elapsed_sec = (current_time - start_time) / 1000000.0;
            float speed = (read_len / 1024.0) / elapsed_sec;
            
            event_data.event = OTA_SERVICE_EVENT_DOWNLOADING;
            event_data.data.download_status.downloaded_bytes = read_len;
            event_data.data.download_status.total_bytes = total_len;
            event_data.data.download_status.progress_percent = (total_len > 0) ? (read_len * 100 / total_len) : 0;
            event_data.data.download_status.speed_kbps = speed;
            
            if (s_config.callback) s_config.callback(&event_data);
            
            last_time = current_time;
            ESP_LOGD(TAG, "Progress: %d%%, Speed: %.2f KB/s", event_data.data.download_status.progress_percent, speed);
        }
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP HTTPS OTA perform failed: %s", esp_err_to_name(err));
        event_data.event = OTA_SERVICE_EVENT_ERROR;
        event_data.data.error_msg = "Download failed";
        if (s_config.callback) s_config.callback(&event_data);
        esp_https_ota_finish(https_ota_handle);
        return err;
    }

    err = esp_https_ota_finish(https_ota_handle);
    if (err == ESP_OK) {
        event_data.event = OTA_SERVICE_EVENT_FINISH;
        if (s_config.callback) s_config.callback(&event_data);
        ESP_LOGI(TAG, "OTA Upgrade successful. Rebooting...");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        esp_restart();
    } else {
        ESP_LOGE(TAG, "ESP HTTPS OTA finish failed: %s", esp_err_to_name(err));
        event_data.event = OTA_SERVICE_EVENT_ERROR;
        event_data.data.error_msg = "Flash finish failed";
        if (s_config.callback) s_config.callback(&event_data);
    }
    return err;
}

static void ota_service_default_event_handler(const ota_service_event_data_t *event_data)
{
    switch (event_data->event) {
        case OTA_SERVICE_EVENT_START_CHECK:
            ESP_LOGI(TAG, "OTA: Checking for update...");
            break;
        case OTA_SERVICE_EVENT_UPDATE_AVAILABLE:
            ESP_LOGI(TAG, "OTA: Update available! Version: %s", event_data->data.update_info.version);
            ESP_LOGI(TAG, "OTA: Description: %s", event_data->data.update_info.description);
            ESP_LOGI(TAG, "OTA: Size: %lu bytes", event_data->data.update_info.total_size);
            if (event_data->data.update_info.mandatory) {
                ESP_LOGW(TAG, "OTA: This is a MANDATORY update!");
            }
            // Automatically start update
            ota_service_start_update();
            break;
        case OTA_SERVICE_EVENT_NO_UPDATE:
            ESP_LOGI(TAG, "OTA: No update available");
            break;
        case OTA_SERVICE_EVENT_START_DOWNLOAD:
            ESP_LOGI(TAG, "OTA: Starting download...");
            break;
        case OTA_SERVICE_EVENT_DOWNLOADING:
            ESP_LOGI(TAG, "OTA: Download progress: %d%%, Speed: %.2f KB/s", 
                     event_data->data.download_status.progress_percent,
                     event_data->data.download_status.speed_kbps);
            break;
        case OTA_SERVICE_EVENT_FINISH:
            ESP_LOGI(TAG, "OTA: Update finished successfully");
            break;
        case OTA_SERVICE_EVENT_ERROR:
            ESP_LOGE(TAG, "OTA: Error: %s", event_data->data.error_msg);
            break;
        default:
            break;
    }
}

typedef struct {
    uint32_t check_interval_ms;
    ota_service_is_connected_cb_t is_connected_cb;
} ota_service_task_context_t;

static void ota_service_task_entry(void *pvParameters)
{
    ota_service_task_context_t *ctx = (ota_service_task_context_t *)pvParameters;
    
    ESP_LOGI(TAG, "OTA task started");
    
    // Initial check (wait for connection first)
    while (ctx->is_connected_cb && !ctx->is_connected_cb()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    // Perform initial check
    ota_service_check_update();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ctx->check_interval_ms));
        
        if (ctx->is_connected_cb && !ctx->is_connected_cb()) {
            continue;
        }
        
        ota_service_check_update();
    }
    
    free(ctx);
    vTaskDelete(NULL);
}

esp_err_t ota_service_start_task(const ota_service_task_config_t *config)
{
    if (!config || !config->check_url) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_service_config_t init_config = {
        .check_url = config->check_url,
        .callback = config->event_callback ? config->event_callback : ota_service_default_event_handler
    };

    esp_err_t err = ota_service_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }

    ota_service_task_context_t *ctx = malloc(sizeof(ota_service_task_context_t));
    if (!ctx) {
        return ESP_ERR_NO_MEM;
    }
    ctx->check_interval_ms = config->check_interval_ms;
    ctx->is_connected_cb = config->is_connected_cb;

    if (xTaskCreate(ota_service_task_entry, "ota_service_task", 8192, ctx, 5, NULL) != pdPASS) {
        free(ctx);
        return ESP_FAIL;
    }

    return ESP_OK;
}
