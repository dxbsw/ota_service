#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ota_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SERVICE_TAG "OTA_SERVICE"

#define OTA_SERVICE_MANIFEST_FILE "manifest.json"
#define OTA_SERVICE_STATE_FILE    "state.json"
#define OTA_SERVICE_INTEGRITY_FILE "integrity.json"
#define OTA_SERVICE_TMP_SUFFIX    ".tmp"
#define OTA_SERVICE_HASH_CHUNK    1024
#define OTA_SERVICE_ERASE_ALIGN   4096
#define OTA_SERVICE_HTTP_BUFFER   512
#define OTA_SERVICE_MAX_RETRY     3
#define OTA_SERVICE_MIN_WORKER_STACK_SIZE 24576
#define OTA_SERVICE_MIN_VERSION   "0.0.0"

/**
 * @brief `ota_service` 组件运行时上下文。
 */
typedef struct {
    ota_service_config_t config;
    bool initialized;
    bool assets_mounted;
    bool assets_meta_mounted;
    bool worker_running;
    bool models_force_ota;
    TaskHandle_t worker_task;
    SemaphoreHandle_t lock;
    ota_service_status_t status;
} ota_service_context_t;

/**
 * @brief 分区完整性记录。
 */
typedef struct {
    char partition[OTA_SERVICE_MAX_PARTITION_LEN];
    char version[OTA_SERVICE_MAX_VERSION_LEN];
    char sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN];
    uint32_t size;
} ota_service_partition_integrity_t;

/**
 * @brief `assets_meta` 中保存的完整性文件对象。
 */
typedef struct {
    ota_service_partition_integrity_t firmware;
    ota_service_partition_integrity_t models;
} ota_service_integrity_info_t;

extern ota_service_context_t g_ota_service_ctx;

/**
 * @brief 安全复制字符串，空指针会写入空串。
 *
 * @param dst 目标缓冲区。
 * @param dst_size 目标缓冲区大小。
 * @param src 源字符串。
 */
void ota_service_copy_string(char *dst, size_t dst_size, const char *src);

/**
 * @brief 将 `manifest` 对象重置为默认值。
 *
 * @param manifest 目标对象。
 */
void ota_service_manifest_set_defaults(ota_service_manifest_t *manifest);

/**
 * @brief 将 `state` 对象重置为默认值。
 *
 * @param state 目标对象。
 */
void ota_service_state_set_defaults(ota_service_update_state_t *state);

/**
 * @brief 将 `integrity` 对象重置为默认值。
 *
 * @param integrity 目标对象。
 */
void ota_service_integrity_set_defaults(ota_service_integrity_info_t *integrity);

/**
 * @brief 资源访问方式转字符串。
 *
 * @param access 访问方式枚举。
 * @return const char* 访问方式字符串。
 */
const char *ota_service_access_to_string(ota_service_resource_access_t access);

/**
 * @brief 从字符串解析资源访问方式。
 *
 * @param access 访问方式字符串。
 * @return ota_service_resource_access_t 访问方式枚举。
 */
ota_service_resource_access_t ota_service_access_from_string(const char *access);

/**
 * @brief 从字符串解析事务状态。
 *
 * @param state 状态字符串。
 * @return ota_service_state_t 状态枚举。
 */
ota_service_state_t ota_service_state_from_string(const char *state);

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
                                      bool format_if_mount_failed);

/**
 * @brief 卸载 LittleFS 分区。
 *
 * @param partition_label 分区标签。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_unmount_partition(const char *partition_label);

/**
 * @brief 获取分区对应的挂载路径。
 *
 * @param partition_label 分区标签。
 * @return const char* 挂载路径，找不到时返回 NULL。
 */
const char *ota_service_get_base_path_for_partition(const char *partition_label);

/**
 * @brief 将逻辑路径拼接为文件系统实际路径。
 *
 * @param output 输出缓冲区。
 * @param output_size 输出缓冲区大小。
 * @param base_path 挂载路径。
 * @param logical_path 逻辑路径。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_build_partition_path(char *output,
                                           size_t output_size,
                                           const char *base_path,
                                           const char *logical_path);

/**
 * @brief 解析 `manifest` JSON 对象。
 *
 * @param root JSON 根对象。
 * @param manifest 输出的清单对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_parse_manifest_json_object(cJSON *root, ota_service_manifest_t *manifest);

/**
 * @brief 原子写文件。
 *
 * @param file_path 目标文件路径。
 * @param data 数据指针。
 * @param size 数据长度。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_write_file_atomic(const char *file_path, const void *data, size_t size);

/**
 * @brief 读取整个文件并分配缓冲区。
 *
 * @param file_path 文件路径。
 * @param buffer_out 输出缓冲区。
 * @param size_out 输出长度。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_read_file_alloc(const char *file_path, char **buffer_out, size_t *size_out);

/**
 * @brief 计算内存数据的 SHA256。
 *
 * @param data 数据指针。
 * @param size 数据长度。
 * @param hex_output 输出十六进制字符串。
 */
void ota_service_compute_memory_sha256(const void *data, size_t size, char *hex_output);

/**
 * @brief 计算文件的 SHA256。
 *
 * @param file_path 文件路径。
 * @param hex_output 输出十六进制字符串。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_compute_file_sha256(const char *file_path, char *hex_output);

/**
 * @brief 计算原始分区区域的 SHA256。
 *
 * @param partition 目标分区。
 * @param offset 偏移。
 * @param size 数据长度。
 * @param hex_output 输出十六进制字符串。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_compute_partition_sha256(const esp_partition_t *partition,
                                               uint32_t offset,
                                               size_t size,
                                               char *hex_output);

/**
 * @brief 获取本地 `manifest.json` 路径。
 *
 * @param path 输出缓冲区。
 * @param path_size 缓冲区大小。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_get_manifest_file_path(char *path, size_t path_size);

/**
 * @brief 获取本地 `state.json` 路径。
 *
 * @param path 输出缓冲区。
 * @param path_size 缓冲区大小。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_get_state_file_path(char *path, size_t path_size);

/**
 * @brief 获取本地 `integrity.json` 路径。
 *
 * @param path 输出缓冲区。
 * @param path_size 缓冲区大小。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_get_integrity_file_path(char *path, size_t path_size);

/**
 * @brief 查找资源在清单中的索引。
 *
 * @param manifest 清单对象。
 * @param name 资源名称。
 * @return int 找到时返回索引，否则返回 -1。
 */
int ota_service_find_manifest_resource_index(const ota_service_manifest_t *manifest, const char *name);

/**
 * @brief 比较两个版本号是否相等。
 *
 * @param current 当前版本。
 * @param target 目标版本。
 * @return true 版本相等。
 * @return false 版本不相等或解析失败。
 */
bool ota_service_version_is_equal_safe(const char *current, const char *target);

/**
 * @brief 判断目标版本是否比当前版本更新。
 *
 * @param current 当前版本。
 * @param target 目标版本。
 * @return true 目标版本更高。
 * @return false 目标版本不更高或解析失败。
 */
bool ota_service_version_is_target_newer(const char *current, const char *target);

/**
 * @brief 判断当前版本是否大于等于给定版本。
 *
 * @param current 当前版本。
 * @param minimum 最低要求版本。
 * @return true 当前版本满足最低要求。
 * @return false 当前版本不满足或解析失败。
 */
bool ota_service_version_is_at_least(const char *current, const char *minimum);

/**
 * @brief 校验本地资源描述是否合法。
 *
 * @param resource 资源对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_validate_resource(const ota_service_local_resource_t *resource);

/**
 * @brief 保存本地 `manifest.json`。
 *
 * @param manifest 清单对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_save_manifest(const ota_service_manifest_t *manifest);

/**
 * @brief 保存本地 `state.json`。
 *
 * @param state 状态对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_save_state(const ota_service_update_state_t *state);

/**
 * @brief 保存本地 `integrity.json`。
 *
 * @param integrity 完整性对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_save_integrity(const ota_service_integrity_info_t *integrity);

/**
 * @brief 读取本地 `integrity.json`。
 *
 * @param integrity 输出完整性对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_load_integrity(ota_service_integrity_info_t *integrity);

/**
 * @brief 重新生成完整性记录中的 models 校验值。
 *
 * @param manifest 本地清单。
 * @param models_integrity 输出 models 完整性记录。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_build_models_integrity(const ota_service_manifest_t *manifest,
                                             ota_service_partition_integrity_t *models_integrity);

/**
 * @brief 判断资源条目是否属于 models 分区。
 *
 * @param entry 资源条目。
 * @return true 属于 models 分区。
 * @return false 不属于 models 分区。
 */
bool ota_service_resource_entry_is_models(const ota_service_resource_entry_t *entry);

/**
 * @brief 比较资源元数据是否变化。
 *
 * @param current_entry 当前资源。
 * @param target_entry 目标资源。
 * @return true 元数据有变化。
 * @return false 元数据无变化。
 */
bool ota_service_resource_entry_metadata_changed(const ota_service_resource_entry_t *current_entry,
                                                 const ota_service_resource_entry_t *target_entry);

/**
 * @brief 获取用于下载的缓冲区大小。
 *
 * @return size_t 缓冲区大小。
 */
size_t ota_service_get_download_buffer_size(void);

/**
 * @brief 尝试优先从 PSRAM 分配缓冲区。
 *
 * @param size 申请大小。
 * @return void* 成功返回指针，失败返回 NULL。
 */
void *ota_service_malloc_prefer_psram(size_t size);

/**
 * @brief 释放通过 `ota_service_malloc_prefer_psram` 申请的内存。
 *
 * @param ptr 内存指针。
 */
void ota_service_free_buffer(void *ptr);

/**
 * @brief 更新运行状态。
 *
 * @param runtime_state 运行阶段。
 * @param transaction_state 事务状态。
 * @param current_target 当前目标。
 * @param current_version 当前版本。
 * @param target_version 目标版本。
 */
void ota_service_set_status_stage(ota_service_runtime_state_t runtime_state,
                                  ota_service_state_t transaction_state,
                                  const char *current_target,
                                  const char *current_version,
                                  const char *target_version);

/**
 * @brief 更新进度信息。
 *
 * @param total_steps 总步骤数。
 * @param completed_steps 已完成步骤数。
 */
void ota_service_set_status_progress(uint32_t total_steps, uint32_t completed_steps);

/**
 * @brief 更新当前文件下载统计信息。
 *
 * @param total_bytes 当前文件总大小。
 * @param downloaded_bytes 当前文件已下载字节数。
 * @param bytes_per_second 当前平均下载速度，单位 B/s。
 */
void ota_service_set_status_transfer(uint32_t total_bytes,
                                     uint32_t downloaded_bytes,
                                     uint32_t bytes_per_second);

/**
 * @brief 清空当前文件下载统计信息。
 */
void ota_service_reset_status_transfer(void);

/**
 * @brief 设置当前更新计划。
 *
 * @param plan 更新计划。
 */
void ota_service_set_status_plan(const ota_service_update_plan_t *plan);

/**
 * @brief 标记 OTA 完成。
 *
 * @param success 是否成功。
 * @param reboot_pending 是否等待重启。
 * @param last_error 错误码。
 */
void ota_service_finish_status(bool success, bool reboot_pending, esp_err_t last_error);

/**
 * @brief 重置当前对外状态对象。
 */
void ota_service_reset_status(void);

/**
 * @brief OTA 后台任务入口。
 *
 * @param arg 未使用参数。
 */
void ota_service_worker_task(void *arg);

#ifdef __cplusplus
}
#endif
