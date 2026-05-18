#include "ota_service_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "version_checker.h"

static void ota_service_partition_integrity_reset(ota_service_partition_integrity_t *record,
                                                  const char *partition_name)
{
    if (record == NULL) {
        return;
    }

    memset(record, 0, sizeof(*record));
    ota_service_copy_string(record->partition, sizeof(record->partition), partition_name);
    ota_service_copy_string(record->version, sizeof(record->version), OTA_SERVICE_MIN_VERSION);
}

/**
 * @brief 递归创建父目录。
 *
 * @param file_path 目标文件路径。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_make_parent_dirs(const char *file_path)
{
    char temp[256] = {0};
    size_t len = 0;

    if (file_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    len = strlen(file_path);
    if (len >= sizeof(temp)) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(temp, file_path, len + 1);

    for (char *cursor = temp + 1; *cursor != 0; cursor++) {
        if (*cursor != '/') {
            continue;
        }

        *cursor = 0;
        if (mkdir(temp, 0775) != 0 && errno != EEXIST) {
            ESP_LOGE(OTA_SERVICE_TAG, "mkdir(%s) failed, errno=%d", temp, errno);
            return ESP_FAIL;
        }
        *cursor = '/';
    }

    return ESP_OK;
}

esp_err_t ota_service_parse_manifest_json_object(cJSON *root, ota_service_manifest_t *manifest)
{
    cJSON *resources = NULL;
    cJSON *item = NULL;

    if (root == NULL || manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_service_manifest_set_defaults(manifest);

    {
        cJSON *format_version = cJSON_GetObjectItem(root, "format_version");
        if (cJSON_IsNumber(format_version)) {
            manifest->format_version = (uint32_t)format_version->valuedouble;
        }
    }

    ota_service_copy_string(manifest->firmware_compat,
                            sizeof(manifest->firmware_compat),
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "firmware_compat")));
    ota_service_copy_string(manifest->manifest_version,
                            sizeof(manifest->manifest_version),
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "manifest_version")));

    resources = cJSON_GetObjectItem(root, "resources");
    if (!cJSON_IsArray(resources)) {
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, resources) {
        ota_service_resource_entry_t *entry = NULL;
        cJSON *offset_item = NULL;
        cJSON *size_item = NULL;
        cJSON *reserved_size_item = NULL;

        if (manifest->resource_count >= OTA_SERVICE_MAX_RESOURCES) {
            return ESP_ERR_INVALID_SIZE;
        }

        entry = &manifest->resources[manifest->resource_count++];
        memset(entry, 0, sizeof(*entry));

        ota_service_copy_string(entry->name, sizeof(entry->name), cJSON_GetStringValue(cJSON_GetObjectItem(item, "name")));
        ota_service_copy_string(entry->type, sizeof(entry->type), cJSON_GetStringValue(cJSON_GetObjectItem(item, "type")));
        ota_service_copy_string(entry->version, sizeof(entry->version), cJSON_GetStringValue(cJSON_GetObjectItem(item, "version")));
        ota_service_copy_string(entry->partition, sizeof(entry->partition), cJSON_GetStringValue(cJSON_GetObjectItem(item, "partition")));
        entry->access = ota_service_access_from_string(cJSON_GetStringValue(cJSON_GetObjectItem(item, "access")));
        ota_service_copy_string(entry->path, sizeof(entry->path), cJSON_GetStringValue(cJSON_GetObjectItem(item, "path")));

        offset_item = cJSON_GetObjectItem(item, "offset");
        size_item = cJSON_GetObjectItem(item, "size");
        reserved_size_item = cJSON_GetObjectItem(item, "reserved_size");

        if (cJSON_IsNumber(offset_item)) {
            entry->offset = (uint32_t)offset_item->valuedouble;
        }
        if (cJSON_IsNumber(size_item)) {
            entry->size = (uint32_t)size_item->valuedouble;
        }
        if (cJSON_IsNumber(reserved_size_item)) {
            entry->reserved_size = (uint32_t)reserved_size_item->valuedouble;
        }

        ota_service_copy_string(entry->sha256, sizeof(entry->sha256), cJSON_GetStringValue(cJSON_GetObjectItem(item, "sha256")));
        entry->required = cJSON_IsTrue(cJSON_GetObjectItem(item, "required"));
    }

    return ESP_OK;
}

esp_err_t ota_service_write_file_atomic(const char *file_path, const void *data, size_t size)
{
    FILE *file = NULL;
    char tmp_path[256] = {0};
    size_t written = 0;
    esp_err_t err = ESP_OK;

    if (file_path == NULL || (data == NULL && size > 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s%s", file_path, OTA_SERVICE_TMP_SUFFIX) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = ota_service_make_parent_dirs(file_path);
    if (err != ESP_OK) {
        return err;
    }

    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        ESP_LOGE(OTA_SERVICE_TAG, "open tmp file failed: %s", tmp_path);
        return ESP_FAIL;
    }

    written = fwrite(data, 1, size, file);
    fclose(file);

    if (written != size) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    if (rename(tmp_path, file_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ota_service_read_file_alloc(const char *file_path, char **buffer_out, size_t *size_out)
{
    FILE *file = NULL;
    long file_size = 0;
    char *buffer = NULL;
    size_t read_size = 0;

    if (file_path == NULL || buffer_out == NULL || size_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *buffer_out = NULL;
    *size_out = 0;

    file = fopen(file_path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return ESP_FAIL;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    buffer = malloc((size_t)file_size + 1);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    read_size = fread(buffer, 1, (size_t)file_size, file);
    fclose(file);

    if (read_size != (size_t)file_size) {
        free(buffer);
        return ESP_FAIL;
    }

    buffer[file_size] = 0;
    *buffer_out = buffer;
    *size_out = (size_t)file_size;
    return ESP_OK;
}

/**
 * @brief 二进制哈希转为十六进制字符串。
 *
 * @param hash 32 字节哈希值。
 * @param hex_output 输出十六进制字符串。
 */
static void ota_service_sha256_to_hex(const unsigned char *hash, char *hex_output)
{
    static const char hex_table[] = "0123456789abcdef";

    for (size_t index = 0; index < 32; index++) {
        hex_output[index * 2] = hex_table[(hash[index] >> 4) & 0x0F];
        hex_output[index * 2 + 1] = hex_table[hash[index] & 0x0F];
    }
    hex_output[64] = 0;
}

void ota_service_compute_memory_sha256(const void *data, size_t size, char *hex_output)
{
    unsigned char hash[32] = {0};

    mbedtls_sha256((const unsigned char *)data, size, hash, 0);
    ota_service_sha256_to_hex(hash, hex_output);
}

esp_err_t ota_service_compute_file_sha256(const char *file_path, char *hex_output)
{
    FILE *file = NULL;
    unsigned char hash[32] = {0};
    unsigned char buffer[OTA_SERVICE_HASH_CHUNK] = {0};
    size_t read_size = 0;
    mbedtls_sha256_context ctx;

    if (file_path == NULL || hex_output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(file_path, "rb");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    while ((read_size = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        mbedtls_sha256_update(&ctx, buffer, read_size);
    }

    fclose(file);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    ota_service_sha256_to_hex(hash, hex_output);
    return ESP_OK;
}

esp_err_t ota_service_compute_partition_sha256(const esp_partition_t *partition,
                                               uint32_t offset,
                                               size_t size,
                                               char *hex_output)
{
    uint8_t buffer[OTA_SERVICE_HASH_CHUNK] = {0};
    unsigned char hash[32] = {0};
    mbedtls_sha256_context ctx;
    size_t processed = 0;

    if (partition == NULL || hex_output == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    while (processed < size) {
        size_t chunk_size = size - processed;
        esp_err_t err;

        if (chunk_size > sizeof(buffer)) {
            chunk_size = sizeof(buffer);
        }

        err = esp_partition_read(partition, offset + processed, buffer, chunk_size);
        if (err != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            return err;
        }

        mbedtls_sha256_update(&ctx, buffer, chunk_size);
        processed += chunk_size;
    }

    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    ota_service_sha256_to_hex(hash, hex_output);
    return ESP_OK;
}

esp_err_t ota_service_get_manifest_file_path(char *path, size_t path_size)
{
    return ota_service_build_partition_path(path,
                                            path_size,
                                            g_ota_service_ctx.config.assets_meta_base_path,
                                            OTA_SERVICE_MANIFEST_FILE);
}

esp_err_t ota_service_get_state_file_path(char *path, size_t path_size)
{
    return ota_service_build_partition_path(path,
                                            path_size,
                                            g_ota_service_ctx.config.assets_meta_base_path,
                                            OTA_SERVICE_STATE_FILE);
}

esp_err_t ota_service_get_integrity_file_path(char *path, size_t path_size)
{
    return ota_service_build_partition_path(path,
                                            path_size,
                                            g_ota_service_ctx.config.assets_meta_base_path,
                                            OTA_SERVICE_INTEGRITY_FILE);
}

int ota_service_find_manifest_resource_index(const ota_service_manifest_t *manifest, const char *name)
{
    if (manifest == NULL || name == NULL) {
        return -1;
    }

    for (size_t index = 0; index < manifest->resource_count; index++) {
        if (strcmp(manifest->resources[index].name, name) == 0) {
            return (int)index;
        }
    }

    return -1;
}

bool ota_service_version_is_equal_safe(const char *current, const char *target)
{
    return current != NULL && target != NULL && version_is_equal_string(current, target);
}

bool ota_service_version_is_target_newer(const char *current, const char *target)
{
    return current != NULL && target != NULL && version_is_greater_string(target, current);
}

bool ota_service_version_is_at_least(const char *current, const char *minimum)
{
    return current != NULL && minimum != NULL &&
           (version_is_equal_string(current, minimum) || version_is_greater_string(current, minimum));
}

bool ota_service_resource_entry_is_models(const ota_service_resource_entry_t *entry)
{
    return entry != NULL &&
           (entry->access == OTA_SERVICE_RESOURCE_ACCESS_MMAP ||
            strcmp(entry->partition, "models") == 0);
}

esp_err_t ota_service_validate_resource(const ota_service_local_resource_t *resource)
{
    if (resource == NULL || resource->name == NULL || resource->type == NULL || resource->version == NULL ||
        resource->partition == NULL || resource->data == NULL || resource->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (resource->access == OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS) {
        if (resource->path == NULL || resource->path[0] == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (resource->access == OTA_SERVICE_RESOURCE_ACCESS_MMAP) {
        if ((resource->offset % OTA_SERVICE_ERASE_ALIGN) != 0) {
            return ESP_ERR_INVALID_ARG;
        }
        if (resource->reserved_size != 0 && resource->size > resource->reserved_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t ota_service_save_manifest(const ota_service_manifest_t *manifest)
{
    cJSON *root = NULL;
    cJSON *resources = NULL;
    char *json_text = NULL;
    char file_path[256] = {0};
    esp_err_t err = ESP_OK;

    if (manifest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    resources = cJSON_CreateArray();
    if (root == NULL || resources == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    cJSON_AddNumberToObject(root, "format_version", manifest->format_version);
    cJSON_AddStringToObject(root, "firmware_compat", manifest->firmware_compat);
    cJSON_AddStringToObject(root, "manifest_version", manifest->manifest_version);
    cJSON_AddItemToObject(root, "resources", resources);

    for (size_t index = 0; index < manifest->resource_count; index++) {
        const ota_service_resource_entry_t *entry = &manifest->resources[index];
        cJSON *item = cJSON_CreateObject();

        if (item == NULL) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        cJSON_AddStringToObject(item, "name", entry->name);
        cJSON_AddStringToObject(item, "type", entry->type);
        cJSON_AddStringToObject(item, "version", entry->version);
        cJSON_AddStringToObject(item, "partition", entry->partition);
        cJSON_AddStringToObject(item, "access", ota_service_access_to_string(entry->access));
        cJSON_AddNumberToObject(item, "size", entry->size);
        cJSON_AddStringToObject(item, "sha256", entry->sha256);
        cJSON_AddBoolToObject(item, "required", entry->required);

        if (entry->path[0] != 0) {
            cJSON_AddStringToObject(item, "path", entry->path);
        }

        if (entry->access == OTA_SERVICE_RESOURCE_ACCESS_MMAP) {
            cJSON_AddNumberToObject(item, "offset", entry->offset);
            cJSON_AddNumberToObject(item, "reserved_size", entry->reserved_size);
        }

        cJSON_AddItemToArray(resources, item);
    }

    json_text = cJSON_Print(root);
    if (json_text == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = ota_service_get_manifest_file_path(file_path, sizeof(file_path));
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = ota_service_write_file_atomic(file_path, json_text, strlen(json_text));

cleanup:
    if (json_text != NULL) {
        cJSON_free(json_text);
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return err;
}

esp_err_t ota_service_save_state(const ota_service_update_state_t *state)
{
    cJSON *root = NULL;
    char *json_text = NULL;
    char file_path[256] = {0};
    esp_err_t err = ESP_OK;

    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "state", ota_service_state_to_string(state->state));
    cJSON_AddStringToObject(root, "target_manifest_version", state->target_manifest_version);
    cJSON_AddStringToObject(root, "target_resource", state->target_resource);
    cJSON_AddStringToObject(root, "target_partition", state->target_partition);
    cJSON_AddStringToObject(root, "target_sha256", state->target_sha256);
    cJSON_AddNumberToObject(root, "retry_count", state->retry_count);

    json_text = cJSON_Print(root);
    if (json_text == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    err = ota_service_get_state_file_path(file_path, sizeof(file_path));
    if (err == ESP_OK) {
        err = ota_service_write_file_atomic(file_path, json_text, strlen(json_text));
    }

    cJSON_free(json_text);
    cJSON_Delete(root);
    return err;
}

static esp_err_t ota_service_add_integrity_json_object(cJSON *root,
                                                       const char *key,
                                                       const ota_service_partition_integrity_t *record)
{
    cJSON *item = NULL;

    if (root == NULL || key == NULL || record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    item = cJSON_CreateObject();
    if (item == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(item, "partition", record->partition);
    cJSON_AddStringToObject(item, "version", record->version);
    cJSON_AddStringToObject(item, "sha256", record->sha256);
    cJSON_AddNumberToObject(item, "size", record->size);
    cJSON_AddItemToObject(root, key, item);
    return ESP_OK;
}

static void ota_service_parse_integrity_json_object(cJSON *root,
                                                    const char *key,
                                                    ota_service_partition_integrity_t *record)
{
    cJSON *item = NULL;
    cJSON *size_item = NULL;

    if (root == NULL || key == NULL || record == NULL) {
        return;
    }

    item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsObject(item)) {
        return;
    }

    ota_service_copy_string(record->partition,
                            sizeof(record->partition),
                            cJSON_GetStringValue(cJSON_GetObjectItem(item, "partition")));
    ota_service_copy_string(record->version,
                            sizeof(record->version),
                            cJSON_GetStringValue(cJSON_GetObjectItem(item, "version")));
    ota_service_copy_string(record->sha256,
                            sizeof(record->sha256),
                            cJSON_GetStringValue(cJSON_GetObjectItem(item, "sha256")));

    size_item = cJSON_GetObjectItem(item, "size");
    if (cJSON_IsNumber(size_item)) {
        record->size = (uint32_t)size_item->valuedouble;
    }
}

esp_err_t ota_service_save_integrity(const ota_service_integrity_info_t *integrity)
{
    cJSON *root = NULL;
    char *json_text = NULL;
    char file_path[256] = {0};
    esp_err_t err = ESP_OK;

    if (integrity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = ota_service_add_integrity_json_object(root, "firmware", &integrity->firmware);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = ota_service_add_integrity_json_object(root, "models", &integrity->models);
    if (err != ESP_OK) {
        goto cleanup;
    }

    json_text = cJSON_Print(root);
    if (json_text == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = ota_service_get_integrity_file_path(file_path, sizeof(file_path));
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = ota_service_write_file_atomic(file_path, json_text, strlen(json_text));

cleanup:
    if (json_text != NULL) {
        cJSON_free(json_text);
    }
    if (root != NULL) {
        cJSON_Delete(root);
    }
    return err;
}

esp_err_t ota_service_load_manifest(ota_service_manifest_t *manifest)
{
    char file_path[256] = {0};
    char *file_buffer = NULL;
    size_t file_size = 0;
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    if (!g_ota_service_ctx.initialized || manifest == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ota_service_manifest_set_defaults(manifest);

    err = ota_service_get_manifest_file_path(file_path, sizeof(file_path));
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_read_file_alloc(file_path, &file_buffer, &file_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_ParseWithLength(file_buffer, file_size);
    if (root == NULL) {
        err = ESP_FAIL;
        goto cleanup;
    }

    err = ota_service_parse_manifest_json_object(root, manifest);

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(file_buffer);
    return err;
}

esp_err_t ota_service_load_state(ota_service_update_state_t *state)
{
    char file_path[256] = {0};
    char *file_buffer = NULL;
    size_t file_size = 0;
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    if (!g_ota_service_ctx.initialized || state == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ota_service_state_set_defaults(state);

    err = ota_service_get_state_file_path(file_path, sizeof(file_path));
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_read_file_alloc(file_path, &file_buffer, &file_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_ParseWithLength(file_buffer, file_size);
    if (root == NULL) {
        err = ESP_FAIL;
        goto cleanup;
    }

    state->state = ota_service_state_from_string(cJSON_GetStringValue(cJSON_GetObjectItem(root, "state")));
    ota_service_copy_string(state->target_manifest_version,
                            sizeof(state->target_manifest_version),
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "target_manifest_version")));
    ota_service_copy_string(state->target_resource,
                            sizeof(state->target_resource),
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "target_resource")));
    ota_service_copy_string(state->target_partition,
                            sizeof(state->target_partition),
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "target_partition")));
    ota_service_copy_string(state->target_sha256,
                            sizeof(state->target_sha256),
                            cJSON_GetStringValue(cJSON_GetObjectItem(root, "target_sha256")));
    {
        cJSON *retry_count_item = cJSON_GetObjectItem(root, "retry_count");
        if (cJSON_IsNumber(retry_count_item)) {
            state->retry_count = (uint32_t)retry_count_item->valuedouble;
        }
    }

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(file_buffer);
    return err;
}

esp_err_t ota_service_load_integrity(ota_service_integrity_info_t *integrity)
{
    char file_path[256] = {0};
    char *file_buffer = NULL;
    size_t file_size = 0;
    cJSON *root = NULL;
    esp_err_t err = ESP_OK;

    if (!g_ota_service_ctx.initialized || integrity == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ota_service_integrity_set_defaults(integrity);

    err = ota_service_get_integrity_file_path(file_path, sizeof(file_path));
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_read_file_alloc(file_path, &file_buffer, &file_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_ParseWithLength(file_buffer, file_size);
    if (root == NULL) {
        err = ESP_FAIL;
        goto cleanup;
    }

    ota_service_parse_integrity_json_object(root, "firmware", &integrity->firmware);
    ota_service_parse_integrity_json_object(root, "models", &integrity->models);

cleanup:
    if (root != NULL) {
        cJSON_Delete(root);
    }
    free(file_buffer);
    return err;
}

esp_err_t ota_service_build_models_integrity(const ota_service_manifest_t *manifest,
                                             ota_service_partition_integrity_t *models_integrity)
{
    const esp_partition_t *partition = NULL;
    mbedtls_sha256_context sha_ctx;
    unsigned char hash[32] = {0};
    char metadata_line[256] = {0};
    char actual_sha[OTA_SERVICE_MAX_SHA256_HEX_LEN] = {0};
    bool has_models = false;
    esp_err_t err = ESP_OK;

    if (manifest == NULL || models_integrity == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ota_service_partition_integrity_reset(models_integrity, "models");

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    for (size_t index = 0; index < manifest->resource_count; index++) {
        const ota_service_resource_entry_t *entry = &manifest->resources[index];

        if (!ota_service_resource_entry_is_models(entry) || entry->size == 0) {
            continue;
        }

        if (partition == NULL) {
            partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 ESP_PARTITION_SUBTYPE_ANY,
                                                 entry->partition);
            if (partition == NULL) {
                err = ESP_ERR_NOT_FOUND;
                goto cleanup;
            }
            ota_service_copy_string(models_integrity->partition,
                                    sizeof(models_integrity->partition),
                                    entry->partition);
        }

        err = ota_service_compute_partition_sha256(partition, entry->offset, entry->size, actual_sha);
        if (err != ESP_OK) {
            goto cleanup;
        }

        snprintf(metadata_line,
                 sizeof(metadata_line),
                 "%s|%s|%u|%u|%u|%s|",
                 entry->name,
                 entry->version,
                 (unsigned int)entry->offset,
                 (unsigned int)entry->size,
                 (unsigned int)entry->reserved_size,
                 actual_sha);
        mbedtls_sha256_update(&sha_ctx,
                              (const unsigned char *)metadata_line,
                              strlen(metadata_line));

        if (strcmp(models_integrity->version, OTA_SERVICE_MIN_VERSION) == 0 ||
            ota_service_version_is_target_newer(models_integrity->version, entry->version)) {
            ota_service_copy_string(models_integrity->version,
                                    sizeof(models_integrity->version),
                                    entry->version);
        }

        models_integrity->size += entry->size;
        has_models = true;
    }

cleanup:
    if (err == ESP_OK && has_models) {
        mbedtls_sha256_finish(&sha_ctx, hash);
        ota_service_sha256_to_hex(hash, models_integrity->sha256);
    }
    mbedtls_sha256_free(&sha_ctx);
    return err;
}

esp_err_t ota_service_prepare_storage(void)
{
    ota_service_manifest_t manifest;
    ota_service_update_state_t state;
    ota_service_integrity_info_t integrity;
    esp_err_t err = ESP_OK;

    if (!g_ota_service_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    err = ota_service_load_manifest(&manifest);
    if (err != ESP_OK) {
        ota_service_manifest_set_defaults(&manifest);
        err = ota_service_save_manifest(&manifest);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = ota_service_load_state(&state);
    if (err != ESP_OK) {
        ota_service_state_set_defaults(&state);
        err = ota_service_save_state(&state);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = ota_service_load_integrity(&integrity);
    if (err != ESP_OK) {
        ota_service_integrity_set_defaults(&integrity);
        err = ota_service_save_integrity(&integrity);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

bool ota_service_resource_entry_metadata_changed(const ota_service_resource_entry_t *current_entry,
                                                 const ota_service_resource_entry_t *target_entry)
{
    bool metadata_changed = false;

    if (current_entry == NULL || target_entry == NULL) {
        return true;
    }

    metadata_changed = current_entry->access != target_entry->access ||
                       current_entry->size != target_entry->size ||
                       strcmp(current_entry->partition, target_entry->partition) != 0 ||
                       strcmp(current_entry->sha256, target_entry->sha256) != 0;

    if (!metadata_changed && target_entry->access == OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS) {
        metadata_changed = strcmp(current_entry->path, target_entry->path) != 0;
    }

    if (!metadata_changed && target_entry->access == OTA_SERVICE_RESOURCE_ACCESS_MMAP) {
        metadata_changed = current_entry->offset != target_entry->offset ||
                           current_entry->reserved_size != target_entry->reserved_size;
    }

    return metadata_changed;
}
