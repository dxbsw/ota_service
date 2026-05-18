#include "ota_service_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

/**
 * @brief OTA 下载上下文。
 */
typedef struct {
    esp_http_client_handle_t client;
    uint8_t *buffer;
    size_t buffer_size;
} ota_service_download_context_t;

/**
 * @brief 二进制哈希转十六进制字符串。
 *
 * @param hash 32 字节哈希。
 * @param hex_output 输出字符串。
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

/**
 * @brief 从 URL 中提取用于日志展示的文件名。
 *
 * @param url 下载地址。
 * @return const char* 文件名；提取失败时返回原始 URL。
 */
static const char *ota_service_get_url_display_name(const char *url)
{
    const char *name = NULL;

    if (url == NULL || url[0] == 0) {
        return "firmware";
    }

    name = strrchr(url, '/');
    if (name == NULL || name[1] == 0) {
        return url;
    }

    return name + 1;
}

static esp_err_t ota_service_persist_integrity(const ota_service_partition_integrity_t *firmware_record,
                                               const ota_service_manifest_t *manifest)
{
    ota_service_integrity_info_t integrity;
    esp_err_t err = ota_service_load_integrity(&integrity);

    if (err != ESP_OK) {
        ota_service_integrity_set_defaults(&integrity);
    }

    if (firmware_record != NULL) {
        integrity.firmware = *firmware_record;
    }

    if (manifest != NULL) {
        err = ota_service_build_models_integrity(manifest, &integrity.models);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ota_service_save_integrity(&integrity);
}

/**
 * @brief 更新当前下载进度与平均速度。
 *
 * @param total_bytes 当前文件总大小。
 * @param downloaded_bytes 已下载字节数。
 * @param start_time_us 开始下载时间，单位 us。
 */
static void ota_service_report_transfer_progress(size_t total_bytes,
                                                 size_t downloaded_bytes,
                                                 int64_t start_time_us)
{
    int64_t elapsed_us = esp_timer_get_time() - start_time_us;
    uint32_t speed_bytes_per_sec = 0;

    if (elapsed_us > 0) {
        uint64_t scaled_speed = ((uint64_t)downloaded_bytes * 1000000ULL) / (uint64_t)elapsed_us;
        if (scaled_speed > UINT32_MAX) {
            speed_bytes_per_sec = UINT32_MAX;
        } else {
            speed_bytes_per_sec = (uint32_t)scaled_speed;
        }
    }

    ota_service_set_status_transfer((uint32_t)total_bytes,
                                    (uint32_t)downloaded_bytes,
                                    speed_bytes_per_sec);
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
            return ESP_FAIL;
        }
        *cursor = '/';
    }

    return ESP_OK;
}

/**
 * @brief 根据元数据地址拼接资源下载地址。
 *
 * @param output 输出 URL。
 * @param output_size 输出缓冲区大小。
 * @param metadata_url 元数据地址。
 * @param entry 资源索引项。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_build_resource_url(char *output,
                                                size_t output_size,
                                                const char *metadata_url,
                                                const ota_service_resource_entry_t *entry)
{
    const char *scheme_pos = NULL;
    const char *path_pos = NULL;
    const char *route_prefix = "/assets";
    size_t base_len = 0;

    if (output == NULL || metadata_url == NULL || entry == NULL || entry->path[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (entry->access == OTA_SERVICE_RESOURCE_ACCESS_MMAP ||
        strcmp(entry->partition, "models") == 0) {
        route_prefix = "/models";
    }

    scheme_pos = strstr(metadata_url, "://");
    path_pos = scheme_pos != NULL ? strchr(scheme_pos + 3, '/') : strchr(metadata_url, '/');
    base_len = path_pos != NULL ? (size_t)(path_pos - metadata_url) : strlen(metadata_url);

    if (snprintf(output, output_size, "%.*s%s%s", (int)base_len, metadata_url, route_prefix, entry->path) >=
        (int)output_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/**
 * @brief 构造 HTTP 客户端配置。
 *
 * @param config HTTP 客户端配置。
 * @param url 请求地址。
 */
static void ota_service_fill_http_config(esp_http_client_config_t *config, const char *url)
{
    memset(config, 0, sizeof(*config));
    config->url = url;
    config->timeout_ms = (int)(g_ota_service_ctx.config.http_timeout_ms != 0 ?
                                   g_ota_service_ctx.config.http_timeout_ms :
                                   5000);
    config->cert_pem = g_ota_service_ctx.config.server_cert_pem;
    config->skip_cert_common_name_check = g_ota_service_ctx.config.skip_cert_common_name_check;
}

/**
 * @brief 打开下载上下文并准备 PSRAM 缓冲区。
 *
 * @param url 下载地址。
 * @param context 输出上下文。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_open_download(const char *url, ota_service_download_context_t *context)
{
    esp_http_client_config_t http_config;
    esp_err_t err = ESP_OK;
    int status_code = 0;

    if (url == NULL || context == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(context, 0, sizeof(*context));
    ota_service_fill_http_config(&http_config, url);

    context->buffer_size = ota_service_get_download_buffer_size();
    context->buffer = ota_service_malloc_prefer_psram(context->buffer_size);
    if (context->buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    context->client = esp_http_client_init(&http_config);
    if (context->client == NULL) {
        ota_service_free_buffer(context->buffer);
        return ESP_FAIL;
    }

    err = esp_http_client_open(context->client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(context->client);
        ota_service_free_buffer(context->buffer);
        return err;
    }

    status_code = esp_http_client_fetch_headers(context->client);
    if (status_code < 0) {
        esp_http_client_close(context->client);
        esp_http_client_cleanup(context->client);
        ota_service_free_buffer(context->buffer);
        return ESP_FAIL;
    }

    if (esp_http_client_get_status_code(context->client) != 200) {
        esp_http_client_close(context->client);
        esp_http_client_cleanup(context->client);
        ota_service_free_buffer(context->buffer);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 关闭下载上下文。
 *
 * @param context 下载上下文。
 */
static void ota_service_close_download(ota_service_download_context_t *context)
{
    if (context == NULL) {
        return;
    }

    if (context->client != NULL) {
        esp_http_client_close(context->client);
        esp_http_client_cleanup(context->client);
        context->client = NULL;
    }

    ota_service_free_buffer(context->buffer);
    context->buffer = NULL;
}

/**
 * @brief 写入状态文件。
 *
 * @param state_value 状态值。
 * @param target_name 当前目标。
 * @param target_partition 当前分区。
 * @param target_sha256 当前目标哈希。
 * @param retry_count 重试次数。
 * @param manifest_version 目标 manifest 版本。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_write_state(ota_service_state_t state_value,
                                         const char *target_name,
                                         const char *target_partition,
                                         const char *target_sha256,
                                         uint32_t retry_count,
                                         const char *manifest_version)
{
    ota_service_update_state_t state;

    ota_service_state_set_defaults(&state);
    state.state = state_value;
    state.retry_count = retry_count;
    ota_service_copy_string(state.target_manifest_version,
                            sizeof(state.target_manifest_version),
                            manifest_version);
    ota_service_copy_string(state.target_resource,
                            sizeof(state.target_resource),
                            target_name);
    ota_service_copy_string(state.target_partition,
                            sizeof(state.target_partition),
                            target_partition);
    ota_service_copy_string(state.target_sha256,
                            sizeof(state.target_sha256),
                            target_sha256);
    return ota_service_save_state(&state);
}

/**
 * @brief 流式下载固件并写入 OTA 分区。
 *
 * @param firmware 固件描述。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_perform_firmware_update(const ota_service_firmware_info_t *firmware,
                                                     uint32_t retry_count,
                                                     const esp_partition_t **pending_boot_partition_out,
                                                     ota_service_partition_integrity_t *firmware_integrity_out)
{
    ota_service_download_context_t context;
    const esp_partition_t *update_partition = NULL;
    const char *firmware_name = NULL;
    esp_ota_handle_t ota_handle = 0;
    mbedtls_sha256_context sha_ctx;
    unsigned char hash[32] = {0};
    char actual_sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN] = {0};
    char written_sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN] = {0};
    size_t total_read = 0;
    esp_err_t err = ESP_OK;
    bool ota_started = false;
    int64_t start_time_us = 0;

    if (pending_boot_partition_out != NULL) {
        *pending_boot_partition_out = NULL;
    }
    if (firmware_integrity_out != NULL) {
        ota_service_partition_integrity_t empty_record = {0};
        *firmware_integrity_out = empty_record;
    }

    if (firmware == NULL || !firmware->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    firmware_name = ota_service_get_url_display_name(firmware->url);

    ota_service_set_status_stage(OTA_SERVICE_RUNTIME_FIRMWARE_OTA,
                                 OTA_SERVICE_STATE_DOWNLOADING,
                                 firmware_name,
                                 NULL,
                                 firmware->version);
    err = ota_service_write_state(OTA_SERVICE_STATE_DOWNLOADING,
                                  firmware_name,
                                  "ota",
                                  firmware->sha256,
                                  retry_count,
                                  NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_open_download(firmware->url, &context);
    if (err != ESP_OK) {
        return err;
    }

    ota_service_set_status_transfer(firmware->size, 0, 0);

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ota_service_close_download(&context);
        return ESP_ERR_NOT_FOUND;
    }

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ota_service_close_download(&context);
        return err;
    }
    ota_started = true;

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    ota_service_set_status_stage(OTA_SERVICE_RUNTIME_FIRMWARE_OTA,
                                 OTA_SERVICE_STATE_WRITING,
                                 firmware_name,
                                 NULL,
                                 firmware->version);
    err = ota_service_write_state(OTA_SERVICE_STATE_WRITING,
                                  firmware_name,
                                  "ota",
                                  firmware->sha256,
                                  retry_count,
                                  NULL);
    if (err != ESP_OK) {
        ota_service_close_download(&context);
        esp_ota_abort(ota_handle);
        return err;
    }
    start_time_us = esp_timer_get_time();

    while (true) {
        int read_len = esp_http_client_read(context.client,
                                            (char *)context.buffer,
                                            (int)context.buffer_size);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }

        err = esp_ota_write(ota_handle, context.buffer, (size_t)read_len);
        if (err != ESP_OK) {
            break;
        }

        mbedtls_sha256_update(&sha_ctx, context.buffer, (size_t)read_len);
        total_read += (size_t)read_len;
        ota_service_report_transfer_progress(firmware->size, total_read, start_time_us);
    }

    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    ota_service_sha256_to_hex(hash, actual_sha256);
    ota_service_close_download(&context);
    ota_service_report_transfer_progress(firmware->size, total_read, start_time_us);

    if (err == ESP_OK && firmware->size != 0 && total_read != firmware->size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && firmware->sha256[0] != 0 && strcmp(actual_sha256, firmware->sha256) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }

    if (err == ESP_OK) {
        ota_service_set_status_stage(OTA_SERVICE_RUNTIME_FIRMWARE_OTA,
                                     OTA_SERVICE_STATE_VERIFYING,
                                     firmware_name,
                                     NULL,
                                     firmware->version);
        err = esp_ota_end(ota_handle);
        ota_started = false;
    }

    if (err == ESP_OK) {
        err = ota_service_compute_partition_sha256(update_partition, 0, total_read, written_sha256);
    }

    if (err == ESP_OK && strcmp(actual_sha256, written_sha256) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }

    if (err == ESP_OK && pending_boot_partition_out != NULL) {
        *pending_boot_partition_out = update_partition;
    }
    if (err == ESP_OK && firmware_integrity_out != NULL) {
        ota_service_copy_string(firmware_integrity_out->partition,
                                sizeof(firmware_integrity_out->partition),
                                update_partition->label);
        ota_service_copy_string(firmware_integrity_out->version,
                                sizeof(firmware_integrity_out->version),
                                firmware->version);
        ota_service_copy_string(firmware_integrity_out->sha256,
                                sizeof(firmware_integrity_out->sha256),
                                written_sha256);
        firmware_integrity_out->size = (uint32_t)total_read;
    }

    if (ota_started) {
        esp_ota_abort(ota_handle);
    }

    return err;
}

/**
 * @brief 将远端资源流式写入 LittleFS 文件。
 *
 * @param url 下载地址。
 * @param entry 资源条目。
 * @param actual_sha256 输出实际哈希。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_download_resource_to_file(const char *url,
                                                       const ota_service_resource_entry_t *entry,
                                                       const char *manifest_version,
                                                       uint32_t retry_count,
                                                       char *actual_sha256)
{
    ota_service_download_context_t context;
    const char *base_path = NULL;
    char file_path[256] = {0};
    char tmp_path[256] = {0};
    FILE *file = NULL;
    mbedtls_sha256_context sha_ctx;
    unsigned char hash[32] = {0};
    size_t total_read = 0;
    esp_err_t err = ESP_OK;
    int64_t start_time_us = 0;

    if (url == NULL || entry == NULL || actual_sha256 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    base_path = ota_service_get_base_path_for_partition(entry->partition);
    if (base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ota_service_build_partition_path(file_path, sizeof(file_path), base_path, entry->path);
    if (err != ESP_OK) {
        return err;
    }

    if (snprintf(tmp_path, sizeof(tmp_path), "%s%s", file_path, OTA_SERVICE_TMP_SUFFIX) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }

    err = ota_service_make_parent_dirs(file_path);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_open_download(url, &context);
    if (err != ESP_OK) {
        return err;
    }

    ota_service_set_status_transfer(entry->size, 0, 0);
    start_time_us = esp_timer_get_time();

    err = ota_service_write_state(OTA_SERVICE_STATE_WRITING,
                                  entry->name,
                                  entry->partition,
                                  entry->sha256,
                                  retry_count,
                                  manifest_version);
    if (err != ESP_OK) {
        ota_service_close_download(&context);
        return err;
    }

    file = fopen(tmp_path, "wb");
    if (file == NULL) {
        ota_service_close_download(&context);
        return ESP_FAIL;
    }

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    while (true) {
        int read_len = esp_http_client_read(context.client, (char *)context.buffer, (int)context.buffer_size);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }

        if (fwrite(context.buffer, 1, (size_t)read_len, file) != (size_t)read_len) {
            err = ESP_FAIL;
            break;
        }

        mbedtls_sha256_update(&sha_ctx, context.buffer, (size_t)read_len);
        total_read += (size_t)read_len;
        ota_service_report_transfer_progress(entry->size, total_read, start_time_us);
    }

    fclose(file);
    file = NULL;
    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    ota_service_sha256_to_hex(hash, actual_sha256);
    ota_service_close_download(&context);
    ota_service_report_transfer_progress(entry->size, total_read, start_time_us);

    if (err == ESP_OK && entry->size != 0 && total_read != entry->size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && entry->sha256[0] != 0 && strcmp(actual_sha256, entry->sha256) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }

    if (err != ESP_OK) {
        unlink(tmp_path);
        return err;
    }

    if (rename(tmp_path, file_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/**
 * @brief 将远端资源流式写入原始分区。
 *
 * @param url 下载地址。
 * @param entry 资源条目。
 * @param actual_sha256 输出实际哈希。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_download_resource_to_partition(const char *url,
                                                            const ota_service_resource_entry_t *entry,
                                                            const char *manifest_version,
                                                            uint32_t retry_count,
                                                            char *actual_sha256)
{
    ota_service_download_context_t context;
    const esp_partition_t *partition = NULL;
    size_t erase_size = 0;
    size_t total_read = 0;
    size_t write_chunk_size = OTA_SERVICE_ERASE_ALIGN;
    mbedtls_sha256_context sha_ctx;
    unsigned char hash[32] = {0};
    esp_err_t err = ESP_OK;
    int64_t start_time_us = 0;

    if (url == NULL || entry == NULL || actual_sha256 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, entry->partition);
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    erase_size = entry->reserved_size != 0 ? entry->reserved_size : entry->size;
    if ((erase_size % OTA_SERVICE_ERASE_ALIGN) != 0) {
        erase_size += OTA_SERVICE_ERASE_ALIGN - (erase_size % OTA_SERVICE_ERASE_ALIGN);
    }

    err = ota_service_open_download(url, &context);
    if (err != ESP_OK) {
        return err;
    }

    ota_service_set_status_transfer(entry->size, 0, 0);
    start_time_us = esp_timer_get_time();

    if (context.buffer_size < write_chunk_size) {
        ota_service_close_download(&context);
        return ESP_ERR_INVALID_SIZE;
    }

    err = ota_service_write_state(OTA_SERVICE_STATE_WRITING,
                                  entry->name,
                                  entry->partition,
                                  entry->sha256,
                                  retry_count,
                                  manifest_version);
    if (err != ESP_OK) {
        ota_service_close_download(&context);
        return err;
    }

    err = esp_partition_erase_range(partition, entry->offset, erase_size);
    if (err != ESP_OK) {
        ota_service_close_download(&context);
        return err;
    }

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    while (true) {
        /* 原始分区资源固定按 4KB 块下载并写入。 */
        int read_len = esp_http_client_read(context.client, (char *)context.buffer, (int)write_chunk_size);
        if (read_len < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            break;
        }

        err = esp_partition_write(partition, entry->offset + total_read, context.buffer, (size_t)read_len);
        if (err != ESP_OK) {
            break;
        }

        mbedtls_sha256_update(&sha_ctx, context.buffer, (size_t)read_len);
        total_read += (size_t)read_len;
        ota_service_report_transfer_progress(entry->size, total_read, start_time_us);
    }

    mbedtls_sha256_finish(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);
    ota_service_sha256_to_hex(hash, actual_sha256);
    ota_service_close_download(&context);
    ota_service_report_transfer_progress(entry->size, total_read, start_time_us);

    if (err == ESP_OK && entry->size != 0 && total_read != entry->size) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK && entry->sha256[0] != 0 && strcmp(actual_sha256, entry->sha256) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }

    return err;
}

/**
 * @brief 执行资源 OTA。
 *
 * @param metadata 远端元数据。
 * @param plan 更新计划。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_perform_assets_update(const ota_service_remote_metadata_t *metadata,
                                                   const ota_service_update_plan_t *plan,
                                                   uint32_t total_steps,
                                                   uint32_t retry_count)
{
    ota_service_manifest_t manifest;
    uint32_t completed_steps = plan->firmware.needs_update ? 1U : 0U;
    esp_err_t err = ota_service_load_manifest(&manifest);

    if (err == ESP_ERR_NOT_FOUND) {
        ota_service_manifest_set_defaults(&manifest);
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    for (size_t index = 0; index < metadata->manifest.resource_count; index++) {
        const ota_service_resource_entry_t *entry = &metadata->manifest.resources[index];
        char resource_url[OTA_SERVICE_MAX_URL_LEN] = {0};
        char actual_sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN] = {0};
        int manifest_index = -1;

        if (!plan->items[index].needs_update) {
            continue;
        }

        err = ota_service_build_resource_url(resource_url,
                                             sizeof(resource_url),
                                             g_ota_service_ctx.config.metadata_url,
                                             entry);
        if (err != ESP_OK) {
            return err;
        }

        ota_service_set_status_stage(OTA_SERVICE_RUNTIME_ASSETS_OTA,
                                     OTA_SERVICE_STATE_DOWNLOADING,
                                     entry->name,
                                     plan->items[index].current_version,
                                     plan->items[index].target_version);
        err = ota_service_write_state(OTA_SERVICE_STATE_DOWNLOADING,
                                      entry->name,
                                      entry->partition,
                                      entry->sha256,
                                      retry_count,
                                      metadata->manifest.manifest_version);
        if (err != ESP_OK) {
            return err;
        }

        if (entry->access == OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS) {
            err = ota_service_download_resource_to_file(resource_url,
                                                        entry,
                                                        metadata->manifest.manifest_version,
                                                        retry_count,
                                                        actual_sha256);
        } else {
            err = ota_service_download_resource_to_partition(resource_url,
                                                             entry,
                                                             metadata->manifest.manifest_version,
                                                             retry_count,
                                                             actual_sha256);
        }
        if (err != ESP_OK) {
            return err;
        }

        ota_service_set_status_stage(OTA_SERVICE_RUNTIME_ASSETS_OTA,
                                     OTA_SERVICE_STATE_VERIFYING,
                                     entry->name,
                                     plan->items[index].current_version,
                                     plan->items[index].target_version);

        manifest_index = ota_service_find_manifest_resource_index(&manifest, entry->name);
        if (manifest_index >= 0) {
            manifest.resources[manifest_index] = *entry;
            ota_service_copy_string(manifest.resources[manifest_index].sha256,
                                    sizeof(manifest.resources[manifest_index].sha256),
                                    actual_sha256);
        } else if (manifest.resource_count < OTA_SERVICE_MAX_RESOURCES) {
            manifest.resources[manifest.resource_count] = *entry;
            ota_service_copy_string(manifest.resources[manifest.resource_count].sha256,
                                    sizeof(manifest.resources[manifest.resource_count].sha256),
                                    actual_sha256);
            manifest.resource_count++;
        } else {
            return ESP_ERR_INVALID_SIZE;
        }

        err = ota_service_save_manifest(&manifest);
        if (err != ESP_OK) {
            return err;
        }

        completed_steps++;
        ota_service_set_status_progress(total_steps, completed_steps);
    }

    ota_service_copy_string(manifest.firmware_compat,
                            sizeof(manifest.firmware_compat),
                            metadata->manifest.firmware_compat);
    ota_service_copy_string(manifest.manifest_version,
                            sizeof(manifest.manifest_version),
                            metadata->manifest.manifest_version);
    manifest.format_version = metadata->manifest.format_version;

    err = ota_service_save_manifest(&manifest);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_persist_integrity(NULL, &manifest);
    if (err != ESP_OK) {
        return err;
    }

    return ota_service_write_state(OTA_SERVICE_STATE_IDLE, NULL, NULL, NULL, 0, metadata->manifest.manifest_version);
}

/**
 * @brief 统计本轮需要执行的总步骤数。
 *
 * @param plan 更新计划。
 * @return uint32_t 步骤数。
 */
static uint32_t ota_service_count_total_steps(const ota_service_update_plan_t *plan)
{
    uint32_t total_steps = plan->firmware.needs_update ? 1U : 0U;

    for (size_t index = 0; index < plan->resource_count; index++) {
        if (plan->items[index].needs_update) {
            total_steps++;
        }
    }

    return total_steps;
}

/**
 * @brief 判断本轮是否存在资源更新。
 *
 * @param plan 更新计划。
 * @return true 存在资源更新。
 * @return false 不存在资源更新。
 */
static bool ota_service_has_asset_updates(const ota_service_update_plan_t *plan)
{
    for (size_t index = 0; index < plan->resource_count; index++) {
        if (plan->items[index].needs_update) {
            return true;
        }
    }

    return false;
}

void ota_service_worker_task(void *arg)
{
    ota_service_remote_metadata_t *metadata = NULL;
    ota_service_update_plan_t *plan = NULL;
    const esp_partition_t *pending_boot_partition = NULL;
    ota_service_partition_integrity_t pending_firmware_integrity = {0};
    esp_err_t err = ESP_OK;
    uint32_t total_steps = 0;
    uint32_t attempt = 0;
    bool reboot_pending = false;

    (void)arg;

    ota_service_set_status_stage(OTA_SERVICE_RUNTIME_CHECKING,
                                 OTA_SERVICE_STATE_IDLE,
                                 NULL,
                                 NULL,
                                 NULL);

    metadata = calloc(1, sizeof(*metadata));
    plan = calloc(1, sizeof(*plan));
    if (metadata == NULL || plan == NULL) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (attempt = 1; attempt <= OTA_SERVICE_MAX_RETRY; attempt++) {
        memset(metadata, 0, sizeof(*metadata));
        memset(plan, 0, sizeof(*plan));
        pending_boot_partition = NULL;
        memset(&pending_firmware_integrity, 0, sizeof(pending_firmware_integrity));
        reboot_pending = false;
        ota_service_set_status_progress(0, 0);
        ota_service_set_status_plan(plan);

        ota_service_set_status_stage(OTA_SERVICE_RUNTIME_CHECKING,
                                     OTA_SERVICE_STATE_IDLE,
                                     NULL,
                                     NULL,
                                     NULL);

        err = ota_service_check_remote_update(metadata, plan);
        if (err != ESP_OK) {
            goto retry_or_fail;
        }

        total_steps = ota_service_count_total_steps(plan);
        ota_service_set_status_progress(total_steps, 0);
        ota_service_set_status_plan(plan);

        if (!plan->firmware.needs_update && total_steps == 0) {
            err = ESP_OK;
            goto success;
        }

        if (plan->firmware.needs_update) {
            err = ota_service_perform_firmware_update(&metadata->firmware,
                                                      attempt,
                                                      &pending_boot_partition,
                                                      &pending_firmware_integrity);
            if (err != ESP_OK) {
                goto retry_or_fail;
            }
            ota_service_set_status_progress(total_steps, 1);
            reboot_pending = true;
        }

        if (ota_service_has_asset_updates(plan)) {
            err = ota_service_perform_assets_update(metadata, plan, total_steps, attempt);
            if (err != ESP_OK) {
                goto retry_or_fail;
            }
            reboot_pending = true;
        }

        if (pending_boot_partition != NULL) {
            err = ota_service_persist_integrity(&pending_firmware_integrity, NULL);
            if (err != ESP_OK) {
                goto retry_or_fail;
            }
            err = esp_ota_set_boot_partition(pending_boot_partition);
            if (err != ESP_OK) {
                goto retry_or_fail;
            }
        }
        goto success;

retry_or_fail:
        ota_service_write_state(OTA_SERVICE_STATE_FAILED,
                                NULL,
                                NULL,
                                NULL,
                                attempt,
                                metadata->has_manifest ? metadata->manifest.manifest_version : NULL);
        ota_service_reset_status_transfer();

        if (attempt < OTA_SERVICE_MAX_RETRY) {
            ESP_LOGW(OTA_SERVICE_TAG,
                     "ota attempt %u/%u failed: %s, retrying",
                     (unsigned int)attempt,
                     (unsigned int)OTA_SERVICE_MAX_RETRY,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        goto cleanup;
    }

success:
    ota_service_finish_status(true, reboot_pending, ESP_OK);

    if (reboot_pending && g_ota_service_ctx.config.auto_reboot) {
        ESP_LOGI(OTA_SERVICE_TAG, "ota finished, rebooting device");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    free(plan);
    free(metadata);
    vTaskDelete(NULL);

cleanup:
    ota_service_finish_status(false, false, err);
    free(plan);
    free(metadata);
    vTaskDelete(NULL);
}
