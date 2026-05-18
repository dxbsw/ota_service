#include "ota_service_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

ota_service_context_t g_ota_service_ctx = {0};

static void ota_service_reset_status_locked(void)
{
    memset(&g_ota_service_ctx.status, 0, sizeof(g_ota_service_ctx.status));
    g_ota_service_ctx.status.runtime_state = OTA_SERVICE_RUNTIME_IDLE;
    g_ota_service_ctx.status.transaction_state = OTA_SERVICE_STATE_IDLE;
}

/**
 * @brief 安全复制字符串，空指针会写入空串。
 *
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param src 源字符串。
 */
void ota_service_copy_string(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = 0;
        return;
    }

    snprintf(dst, dst_size, "%s", src);
}

/**
 * @brief 将 `manifest` 对象重置为默认值。
 *
 * @param manifest 目标对象。
 */
void ota_service_manifest_set_defaults(ota_service_manifest_t *manifest)
{
    if (manifest == NULL) {
        return;
    }

    memset(manifest, 0, sizeof(*manifest));
    manifest->format_version = 2;
    ota_service_copy_string(manifest->firmware_compat,
                            sizeof(manifest->firmware_compat),
                            OTA_SERVICE_MIN_VERSION);
    ota_service_copy_string(manifest->manifest_version,
                            sizeof(manifest->manifest_version),
                            OTA_SERVICE_MIN_VERSION);
}

/**
 * @brief 将 `state` 对象重置为默认值。
 *
 * @param state 目标对象。
 */
void ota_service_state_set_defaults(ota_service_update_state_t *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->state = OTA_SERVICE_STATE_IDLE;
    ota_service_copy_string(state->target_manifest_version,
                            sizeof(state->target_manifest_version),
                            OTA_SERVICE_MIN_VERSION);
}

void ota_service_integrity_set_defaults(ota_service_integrity_info_t *integrity)
{
    if (integrity == NULL) {
        return;
    }

    memset(integrity, 0, sizeof(*integrity));
    ota_service_copy_string(integrity->firmware.partition,
                            sizeof(integrity->firmware.partition),
                            "ota");
    ota_service_copy_string(integrity->firmware.version,
                            sizeof(integrity->firmware.version),
                            OTA_SERVICE_MIN_VERSION);
    ota_service_copy_string(integrity->models.partition,
                            sizeof(integrity->models.partition),
                            "models");
    ota_service_copy_string(integrity->models.version,
                            sizeof(integrity->models.version),
                            OTA_SERVICE_MIN_VERSION);
}

/**
 * @brief 资源访问方式转字符串。
 *
 * @param access 访问方式枚举。
 * @return const char* 访问方式字符串。
 */
const char *ota_service_access_to_string(ota_service_resource_access_t access)
{
    switch (access) {
        case OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS:
            return "littlefs";
        case OTA_SERVICE_RESOURCE_ACCESS_MMAP:
            return "mmap";
        default:
            return "unknown";
    }
}

/**
 * @brief 从字符串解析资源访问方式。
 *
 * @param access 访问方式字符串。
 * @return ota_service_resource_access_t 访问方式枚举。
 */
ota_service_resource_access_t ota_service_access_from_string(const char *access)
{
    if (access != NULL && strcmp(access, "mmap") == 0) {
        return OTA_SERVICE_RESOURCE_ACCESS_MMAP;
    }

    return OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS;
}

const char *ota_service_state_to_string(ota_service_state_t state)
{
    switch (state) {
        case OTA_SERVICE_STATE_IDLE:
            return "idle";
        case OTA_SERVICE_STATE_DOWNLOADING:
            return "downloading";
        case OTA_SERVICE_STATE_WRITING:
            return "writing";
        case OTA_SERVICE_STATE_VERIFYING:
            return "verifying";
        case OTA_SERVICE_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

/**
 * @brief 从字符串解析事务状态。
 *
 * @param state 状态字符串。
 * @return ota_service_state_t 状态枚举。
 */
ota_service_state_t ota_service_state_from_string(const char *state)
{
    if (state == NULL || strcmp(state, "idle") == 0) {
        return OTA_SERVICE_STATE_IDLE;
    }
    if (strcmp(state, "downloading") == 0) {
        return OTA_SERVICE_STATE_DOWNLOADING;
    }
    if (strcmp(state, "writing") == 0) {
        return OTA_SERVICE_STATE_WRITING;
    }
    if (strcmp(state, "verifying") == 0) {
        return OTA_SERVICE_STATE_VERIFYING;
    }
    if (strcmp(state, "failed") == 0) {
        return OTA_SERVICE_STATE_FAILED;
    }

    return OTA_SERVICE_STATE_IDLE;
}

ota_service_config_t ota_service_get_default_config(void)
{
    ota_service_config_t config = {
        .assets_partition_label = "assets",
        .assets_meta_partition_label = "assets_meta",
        .assets_base_path = "/assets",
        .assets_meta_base_path = "/assets_meta",
        .metadata_url = NULL,
        .server_cert_pem = NULL,
        .http_timeout_ms = 5000,
        .worker_stack_size = OTA_SERVICE_MIN_WORKER_STACK_SIZE,
        .worker_priority = 5,
        .download_buffer_size = 8192,
        .skip_cert_common_name_check = false,
        .auto_reboot = false,
        .format_if_mount_failed = true,
    };

    return config;
}

const char *ota_service_get_assets_base_path(void)
{
    return g_ota_service_ctx.config.assets_base_path;
}

const char *ota_service_get_assets_meta_base_path(void)
{
    return g_ota_service_ctx.config.assets_meta_base_path;
}

/**
 * @brief 生成带默认值和约束修正后的有效配置。
 *
 * @param input 用户传入配置，可为 NULL。
 * @param output 输出有效配置。
 */
static void ota_service_fill_effective_config(const ota_service_config_t *input,
                                              ota_service_config_t *output)
{
    ota_service_config_t default_config = ota_service_get_default_config();

    if (output == NULL) {
        return;
    }

    *output = input != NULL ? *input : default_config;

    if (output->assets_partition_label == NULL) {
        output->assets_partition_label = default_config.assets_partition_label;
    }
    if (output->assets_meta_partition_label == NULL) {
        output->assets_meta_partition_label = default_config.assets_meta_partition_label;
    }
    if (output->assets_base_path == NULL) {
        output->assets_base_path = default_config.assets_base_path;
    }
    if (output->assets_meta_base_path == NULL) {
        output->assets_meta_base_path = default_config.assets_meta_base_path;
    }
    if (output->http_timeout_ms == 0) {
        output->http_timeout_ms = default_config.http_timeout_ms;
    }
    if (output->worker_stack_size == 0) {
        output->worker_stack_size = default_config.worker_stack_size;
    } else if (output->worker_stack_size < OTA_SERVICE_MIN_WORKER_STACK_SIZE) {
        output->worker_stack_size = OTA_SERVICE_MIN_WORKER_STACK_SIZE;
    }
    if (output->worker_priority == 0) {
        output->worker_priority = default_config.worker_priority;
    }
    if (output->download_buffer_size == 0) {
        output->download_buffer_size = default_config.download_buffer_size;
    }
}

/**
 * @brief 判断挂载后不可变配置是否发生变化。
 *
 * @param current 当前已生效配置。
 * @param candidate 候选配置。
 * @return true 存在不可热更新字段变化。
 * @return false 可直接复用当前挂载状态。
 */
static bool ota_service_has_mount_config_changed(const ota_service_config_t *current,
                                                 const ota_service_config_t *candidate)
{
    if (current == NULL || candidate == NULL) {
        return true;
    }

    return strcmp(current->assets_partition_label, candidate->assets_partition_label) != 0 ||
           strcmp(current->assets_meta_partition_label, candidate->assets_meta_partition_label) != 0 ||
           strcmp(current->assets_base_path, candidate->assets_base_path) != 0 ||
           strcmp(current->assets_meta_base_path, candidate->assets_meta_base_path) != 0;
}

/**
 * @brief 挂载 LittleFS 分区。
 *
 * @param partition_label 分区标签。
 * @param base_path 挂载路径。
 * @param format_if_mount_failed 失败时是否格式化。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_mount_partition(const char *partition_label,
                                      const char *base_path,
                                      bool format_if_mount_failed)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = base_path,
        .partition_label = partition_label,
        .format_if_mount_failed = format_if_mount_failed,
        .dont_mount = false,
    };

    return esp_vfs_littlefs_register(&conf);
}

/**
 * @brief 卸载 LittleFS 分区。
 *
 * @param partition_label 分区标签。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_unmount_partition(const char *partition_label)
{
    return esp_vfs_littlefs_unregister(partition_label);
}

/**
 * @brief 判断两个分区标签是否相等。
 *
 * @param lhs 左值。
 * @param rhs 右值。
 * @return true 标签相等。
 * @return false 标签不相等。
 */
static bool ota_service_is_partition_name_equal(const char *lhs, const char *rhs)
{
    return lhs != NULL && rhs != NULL && strcmp(lhs, rhs) == 0;
}

const char *ota_service_get_base_path_for_partition(const char *partition_label)
{
    if (ota_service_is_partition_name_equal(partition_label, g_ota_service_ctx.config.assets_partition_label)) {
        return g_ota_service_ctx.config.assets_base_path;
    }

    if (ota_service_is_partition_name_equal(partition_label, g_ota_service_ctx.config.assets_meta_partition_label)) {
        return g_ota_service_ctx.config.assets_meta_base_path;
    }

    return NULL;
}

esp_err_t ota_service_build_partition_path(char *output,
                                           size_t output_size,
                                           const char *base_path,
                                           const char *logical_path)
{
    const char *relative_path = logical_path;

    if (output == NULL || base_path == NULL || logical_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (logical_path[0] == '/') {
        relative_path = logical_path + 1;
    }

    if (relative_path[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(output, output_size, "%s/%s", base_path, relative_path) >= (int)output_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

esp_err_t ota_service_init(const ota_service_config_t *config)
{
    ota_service_config_t effective_config;
    esp_err_t err = ESP_OK;

    if (g_ota_service_ctx.initialized) {
        return ESP_OK;
    }

    ota_service_fill_effective_config(config, &effective_config);
    g_ota_service_ctx.config = effective_config;

    if (g_ota_service_ctx.config.assets_partition_label == NULL ||
        g_ota_service_ctx.config.assets_meta_partition_label == NULL ||
        g_ota_service_ctx.config.assets_base_path == NULL ||
        g_ota_service_ctx.config.assets_meta_base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    g_ota_service_ctx.lock = xSemaphoreCreateMutex();
    if (g_ota_service_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ota_service_reset_status_locked();

    err = ota_service_mount_partition(g_ota_service_ctx.config.assets_partition_label,
                                      g_ota_service_ctx.config.assets_base_path,
                                      g_ota_service_ctx.config.format_if_mount_failed);
    if (err != ESP_OK) {
        vSemaphoreDelete(g_ota_service_ctx.lock);
        g_ota_service_ctx.lock = NULL;
        return err;
    }
    g_ota_service_ctx.assets_mounted = true;

    err = ota_service_mount_partition(g_ota_service_ctx.config.assets_meta_partition_label,
                                      g_ota_service_ctx.config.assets_meta_base_path,
                                      g_ota_service_ctx.config.format_if_mount_failed);
    if (err != ESP_OK) {
        ota_service_unmount_partition(g_ota_service_ctx.config.assets_partition_label);
        g_ota_service_ctx.assets_mounted = false;
        vSemaphoreDelete(g_ota_service_ctx.lock);
        g_ota_service_ctx.lock = NULL;
        return err;
    }

    g_ota_service_ctx.assets_meta_mounted = true;
    g_ota_service_ctx.initialized = true;

    return ota_service_prepare_storage();
}

esp_err_t ota_service_deinit(void)
{
    esp_err_t err = ESP_OK;

    if (!g_ota_service_ctx.initialized) {
        return ESP_OK;
    }

    if (g_ota_service_ctx.assets_meta_mounted) {
        err = ota_service_unmount_partition(g_ota_service_ctx.config.assets_meta_partition_label);
        g_ota_service_ctx.assets_meta_mounted = false;
        if (err != ESP_OK) {
            return err;
        }
    }

    if (g_ota_service_ctx.assets_mounted) {
        err = ota_service_unmount_partition(g_ota_service_ctx.config.assets_partition_label);
        g_ota_service_ctx.assets_mounted = false;
        if (err != ESP_OK) {
            return err;
        }
    }

    if (g_ota_service_ctx.lock != NULL) {
        vSemaphoreDelete(g_ota_service_ctx.lock);
        g_ota_service_ctx.lock = NULL;
    }

    memset(&g_ota_service_ctx, 0, sizeof(g_ota_service_ctx));
    return ESP_OK;
}

size_t ota_service_get_download_buffer_size(void)
{
    return g_ota_service_ctx.config.download_buffer_size != 0 ?
               g_ota_service_ctx.config.download_buffer_size :
               8192;
}

void *ota_service_malloc_prefer_psram(size_t size)
{
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer != NULL) {
        return buffer;
    }
#endif
    return heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void ota_service_free_buffer(void *ptr)
{
    heap_caps_free(ptr);
}

void ota_service_set_status_stage(ota_service_runtime_state_t runtime_state,
                                  ota_service_state_t transaction_state,
                                  const char *current_target,
                                  const char *current_version,
                                  const char *target_version)
{
    if (g_ota_service_ctx.lock == NULL) {
        return;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    g_ota_service_ctx.status.runtime_state = runtime_state;
    g_ota_service_ctx.status.transaction_state = transaction_state;
    g_ota_service_ctx.status.busy = !(runtime_state == OTA_SERVICE_RUNTIME_COMPLETED ||
                                      runtime_state == OTA_SERVICE_RUNTIME_NO_UPDATE ||
                                      runtime_state == OTA_SERVICE_RUNTIME_FAILED ||
                                      runtime_state == OTA_SERVICE_RUNTIME_REBOOT_REQUIRED);
    ota_service_copy_string(g_ota_service_ctx.status.current_target,
                            sizeof(g_ota_service_ctx.status.current_target),
                            current_target);
    ota_service_copy_string(g_ota_service_ctx.status.current_version,
                            sizeof(g_ota_service_ctx.status.current_version),
                            current_version);
    ota_service_copy_string(g_ota_service_ctx.status.target_version,
                            sizeof(g_ota_service_ctx.status.target_version),
                            target_version);
    if (transaction_state == OTA_SERVICE_STATE_IDLE ||
        transaction_state == OTA_SERVICE_STATE_VERIFYING ||
        runtime_state == OTA_SERVICE_RUNTIME_COMPLETED ||
        runtime_state == OTA_SERVICE_RUNTIME_NO_UPDATE ||
        runtime_state == OTA_SERVICE_RUNTIME_FAILED ||
        runtime_state == OTA_SERVICE_RUNTIME_REBOOT_REQUIRED) {
        g_ota_service_ctx.status.current_file_size = 0;
        g_ota_service_ctx.status.current_file_downloaded = 0;
        g_ota_service_ctx.status.current_speed_bytes_per_sec = 0;
    }
    xSemaphoreGive(g_ota_service_ctx.lock);
}

void ota_service_set_status_progress(uint32_t total_steps, uint32_t completed_steps)
{
    if (g_ota_service_ctx.lock == NULL) {
        return;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    g_ota_service_ctx.status.total_steps = total_steps;
    g_ota_service_ctx.status.completed_steps = completed_steps;
    xSemaphoreGive(g_ota_service_ctx.lock);
}

void ota_service_set_status_transfer(uint32_t total_bytes,
                                     uint32_t downloaded_bytes,
                                     uint32_t bytes_per_second)
{
    if (g_ota_service_ctx.lock == NULL) {
        return;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    g_ota_service_ctx.status.current_file_size = total_bytes;
    g_ota_service_ctx.status.current_file_downloaded = downloaded_bytes;
    g_ota_service_ctx.status.current_speed_bytes_per_sec = bytes_per_second;
    xSemaphoreGive(g_ota_service_ctx.lock);
}

void ota_service_reset_status_transfer(void)
{
    ota_service_set_status_transfer(0, 0, 0);
}

void ota_service_set_status_plan(const ota_service_update_plan_t *plan)
{
    bool need_ota = false;
    bool assets_need_update = false;

    if (g_ota_service_ctx.lock == NULL || plan == NULL) {
        return;
    }

    need_ota = plan->firmware.needs_update;
    for (size_t index = 0; index < plan->resource_count; index++) {
        if (plan->items[index].needs_update) {
            need_ota = true;
            assets_need_update = true;
            break;
        }
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    g_ota_service_ctx.status.plan = *plan;
    g_ota_service_ctx.status.need_ota = need_ota;
    g_ota_service_ctx.status.firmware_needs_update = plan->firmware.needs_update;
    g_ota_service_ctx.status.assets_need_update = assets_need_update;
    xSemaphoreGive(g_ota_service_ctx.lock);
}

void ota_service_finish_status(bool success, bool reboot_pending, esp_err_t last_error)
{
    if (g_ota_service_ctx.lock == NULL) {
        return;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    g_ota_service_ctx.status.success = success;
    g_ota_service_ctx.status.completed = true;
    g_ota_service_ctx.status.busy = false;
    g_ota_service_ctx.status.reboot_pending = reboot_pending;
    g_ota_service_ctx.status.last_error = last_error;
    if (reboot_pending) {
        g_ota_service_ctx.status.runtime_state = OTA_SERVICE_RUNTIME_REBOOT_REQUIRED;
    } else if (success && g_ota_service_ctx.status.need_ota) {
        g_ota_service_ctx.status.runtime_state = OTA_SERVICE_RUNTIME_COMPLETED;
    } else if (success) {
        g_ota_service_ctx.status.runtime_state = OTA_SERVICE_RUNTIME_NO_UPDATE;
    } else {
        g_ota_service_ctx.status.runtime_state = OTA_SERVICE_RUNTIME_FAILED;
    }
    g_ota_service_ctx.worker_running = false;
    g_ota_service_ctx.worker_task = NULL;
    xSemaphoreGive(g_ota_service_ctx.lock);
}

void ota_service_reset_status(void)
{
    if (g_ota_service_ctx.lock == NULL) {
        return;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    ota_service_reset_status_locked();
    xSemaphoreGive(g_ota_service_ctx.lock);
}

esp_err_t ota_service_get_status(ota_service_status_t *status)
{
    if (status == NULL || !g_ota_service_ctx.initialized || g_ota_service_ctx.lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    *status = g_ota_service_ctx.status;
    xSemaphoreGive(g_ota_service_ctx.lock);
    return ESP_OK;
}

bool ota_service_is_finished(void)
{
    bool finished = false;

    if (!g_ota_service_ctx.initialized || g_ota_service_ctx.lock == NULL) {
        return false;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    finished = g_ota_service_ctx.status.completed;
    xSemaphoreGive(g_ota_service_ctx.lock);
    return finished;
}

bool ota_service_need_ota(void)
{
    bool need_ota = false;

    if (!g_ota_service_ctx.initialized || g_ota_service_ctx.lock == NULL) {
        return false;
    }

    xSemaphoreTake(g_ota_service_ctx.lock, portMAX_DELAY);
    need_ota = g_ota_service_ctx.status.need_ota;
    xSemaphoreGive(g_ota_service_ctx.lock);
    return need_ota;
}

esp_err_t ota_service_start(const ota_service_config_t *config)
{
    ota_service_config_t effective_config;
    esp_err_t err = ESP_OK;

    ota_service_fill_effective_config(config, &effective_config);

    if (!g_ota_service_ctx.initialized) {
        err = ota_service_init(&effective_config);
    } else {
        if (ota_service_has_mount_config_changed(&g_ota_service_ctx.config, &effective_config)) {
            return ESP_ERR_INVALID_STATE;
        }
        g_ota_service_ctx.config = effective_config;
    }

    if (err != ESP_OK) {
        return err;
    }

    if (g_ota_service_ctx.worker_running) {
        return ESP_ERR_INVALID_STATE;
    }

    ota_service_reset_status();
    ota_service_set_status_stage(OTA_SERVICE_RUNTIME_PREPARING,
                                 OTA_SERVICE_STATE_IDLE,
                                 NULL,
                                 NULL,
                                 NULL);

    if (xTaskCreate(ota_service_worker_task,
                    "ota_service_worker",
                    g_ota_service_ctx.config.worker_stack_size,
                    NULL,
                    g_ota_service_ctx.config.worker_priority,
                    &g_ota_service_ctx.worker_task) != pdPASS) {
        return ESP_FAIL;
    }

    g_ota_service_ctx.worker_running = true;
    return ESP_OK;
}
