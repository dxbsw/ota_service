#include "ota_service_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"

/**
 * @brief HTTP 响应缓冲区。
 */
typedef struct {
    char *buffer;
    size_t size;
    size_t capacity;
} ota_service_http_buffer_t;

/**
 * @brief 处理 HTTP 数据回调，持续拼接响应体。
 *
 * @param evt HTTP 事件。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_http_event_handler(esp_http_client_event_t *evt)
{
    ota_service_http_buffer_t *response = (ota_service_http_buffer_t *)evt->user_data;

    if (evt->event_id != HTTP_EVENT_ON_DATA || response == NULL || evt->data == NULL || evt->data_len <= 0) {
        return ESP_OK;
    }

    if (response->size + (size_t)evt->data_len + 1 > response->capacity) {
        size_t new_capacity = response->capacity == 0 ? OTA_SERVICE_HTTP_BUFFER : response->capacity;
        char *new_buffer = NULL;

        while (new_capacity < response->size + (size_t)evt->data_len + 1) {
            new_capacity *= 2;
        }

        new_buffer = realloc(response->buffer, new_capacity);
        if (new_buffer == NULL) {
            return ESP_ERR_NO_MEM;
        }

        response->buffer = new_buffer;
        response->capacity = new_capacity;
    }

    memcpy(response->buffer + response->size, evt->data, (size_t)evt->data_len);
    response->size += (size_t)evt->data_len;
    response->buffer[response->size] = 0;
    return ESP_OK;
}

/**
 * @brief 从 URL 拉取文本内容。
 *
 * @param url 目标地址。
 * @param json_text_out 输出文本缓冲区，需要外部释放。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_fetch_text_from_url(const char *url, char **json_text_out)
{
    ota_service_http_buffer_t response = {0};
    esp_http_client_config_t http_config = {0};
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;
    int status_code = 0;

    if (url == NULL || json_text_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *json_text_out = NULL;

    http_config.url = url;
    http_config.timeout_ms = (int)(g_ota_service_ctx.config.http_timeout_ms != 0 ?
                                       g_ota_service_ctx.config.http_timeout_ms :
                                       5000);
    http_config.event_handler = ota_service_http_event_handler;
    http_config.user_data = &response;
    http_config.cert_pem = g_ota_service_ctx.config.server_cert_pem;
    http_config.skip_cert_common_name_check = g_ota_service_ctx.config.skip_cert_common_name_check;

    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(OTA_SERVICE_TAG, "http perform failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(OTA_SERVICE_TAG, "http status code invalid: %d", status_code);
        err = ESP_FAIL;
        goto cleanup;
    }

    if (response.buffer == NULL || response.size == 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    *json_text_out = response.buffer;
    response.buffer = NULL;

cleanup:
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    free(response.buffer);
    return err;
}

/**
 * @brief 解析固件 JSON 对象。
 *
 * @param firmware_obj 固件 JSON 对象。
 * @param firmware 输出固件信息。
 */
static void ota_service_parse_firmware_json_object(cJSON *firmware_obj, ota_service_firmware_info_t *firmware)
{
    if (!cJSON_IsObject(firmware_obj) || firmware == NULL) {
        return;
    }

    memset(firmware, 0, sizeof(*firmware));
    ota_service_copy_string(firmware->version,
                            sizeof(firmware->version),
                            cJSON_GetStringValue(cJSON_GetObjectItem(firmware_obj, "version")));
    ota_service_copy_string(firmware->url,
                            sizeof(firmware->url),
                            cJSON_GetStringValue(cJSON_GetObjectItem(firmware_obj, "url")));
    ota_service_copy_string(firmware->sha256,
                            sizeof(firmware->sha256),
                            cJSON_GetStringValue(cJSON_GetObjectItem(firmware_obj, "sha256")));
    ota_service_copy_string(firmware->description,
                            sizeof(firmware->description),
                            cJSON_GetStringValue(cJSON_GetObjectItem(firmware_obj, "description")));

    {
        cJSON *size_item = cJSON_GetObjectItem(firmware_obj, "size");
        if (cJSON_IsNumber(size_item)) {
            firmware->size = (uint32_t)size_item->valuedouble;
        }
    }

    firmware->mandatory = cJSON_IsTrue(cJSON_GetObjectItem(firmware_obj, "mandatory"));
    firmware->valid = firmware->version[0] != 0 && firmware->url[0] != 0;
}

esp_err_t ota_service_parse_remote_metadata(const char *json_text,
                                            ota_service_remote_metadata_t *metadata)
{
    cJSON *root = NULL;
    cJSON *firmware_obj = NULL;
    cJSON *manifest_obj = NULL;
    esp_err_t err = ESP_OK;

    if (json_text == NULL || metadata == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(metadata, 0, sizeof(*metadata));
    metadata->protocol_version = 1;

    root = cJSON_Parse(json_text);
    if (root == NULL) {
        return ESP_FAIL;
    }

    {
        cJSON *protocol_version = cJSON_GetObjectItem(root, "protocol_version");
        if (cJSON_IsNumber(protocol_version)) {
            metadata->protocol_version = (uint32_t)protocol_version->valuedouble;
        }
    }

    firmware_obj = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware_obj)) {
        ota_service_parse_firmware_json_object(firmware_obj, &metadata->firmware);
        metadata->has_firmware = true;
    }

    manifest_obj = cJSON_GetObjectItem(root, "manifest");
    if (!cJSON_IsObject(manifest_obj)) {
        manifest_obj = cJSON_GetObjectItem(root, "assets");
    }
    if (cJSON_IsObject(manifest_obj)) {
        err = ota_service_parse_manifest_json_object(manifest_obj, &metadata->manifest);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return err;
        }
        metadata->has_manifest = true;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t ota_service_fetch_remote_metadata(ota_service_remote_metadata_t *metadata)
{
    char *json_text = NULL;
    esp_err_t err = ESP_OK;

    if (!g_ota_service_ctx.initialized || metadata == NULL ||
        g_ota_service_ctx.config.metadata_url == NULL ||
        g_ota_service_ctx.config.metadata_url[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ota_service_fetch_text_from_url(g_ota_service_ctx.config.metadata_url, &json_text);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_parse_remote_metadata(json_text, metadata);
    free(json_text);
    return err;
}

esp_err_t ota_service_check_remote_update(ota_service_remote_metadata_t *metadata_out,
                                          ota_service_update_plan_t *plan)
{
    ota_service_remote_metadata_t metadata;
    esp_err_t err = ESP_OK;

    if (plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ota_service_fetch_remote_metadata(&metadata);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_build_remote_update_plan(&metadata, plan);
    if (err != ESP_OK) {
        return err;
    }

    if (metadata_out != NULL) {
        *metadata_out = metadata;
    }

    return ESP_OK;
}
