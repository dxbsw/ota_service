#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SERVICE_MAX_RESOURCES           16
#define OTA_SERVICE_MAX_VERSION_LEN         16
#define OTA_SERVICE_MAX_NAME_LEN            32
#define OTA_SERVICE_MAX_TYPE_LEN            16
#define OTA_SERVICE_MAX_PARTITION_LEN       16
#define OTA_SERVICE_MAX_PATH_LEN            128
#define OTA_SERVICE_MAX_SHA256_HEX_LEN      65
#define OTA_SERVICE_MAX_COMPAT_LEN          32
#define OTA_SERVICE_MAX_URL_LEN             256
#define OTA_SERVICE_MAX_DESC_LEN            128

/**
 * @brief 资源访问方式。
 */
typedef enum {
    OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS = 0,
    OTA_SERVICE_RESOURCE_ACCESS_MMAP,
} ota_service_resource_access_t;

/**
 * @brief 更新事务状态。
 */
typedef enum {
    OTA_SERVICE_STATE_IDLE = 0,
    OTA_SERVICE_STATE_DOWNLOADING,
    OTA_SERVICE_STATE_WRITING,
    OTA_SERVICE_STATE_VERIFYING,
    OTA_SERVICE_STATE_FAILED,
} ota_service_state_t;

/**
 * @brief OTA 运行阶段。
 */
typedef enum {
    OTA_SERVICE_RUNTIME_IDLE = 0,
    OTA_SERVICE_RUNTIME_PREPARING,
    OTA_SERVICE_RUNTIME_CHECKING,
    OTA_SERVICE_RUNTIME_FIRMWARE_OTA,
    OTA_SERVICE_RUNTIME_ASSETS_OTA,
    OTA_SERVICE_RUNTIME_COMPLETED,
    OTA_SERVICE_RUNTIME_NO_UPDATE,
    OTA_SERVICE_RUNTIME_FAILED,
    OTA_SERVICE_RUNTIME_REBOOT_REQUIRED,
} ota_service_runtime_state_t;

/**
 * @brief 资源索引项。
 */
typedef struct {
    char name[OTA_SERVICE_MAX_NAME_LEN];
    char type[OTA_SERVICE_MAX_TYPE_LEN];
    char version[OTA_SERVICE_MAX_VERSION_LEN];
    char partition[OTA_SERVICE_MAX_PARTITION_LEN];
    ota_service_resource_access_t access;
    char path[OTA_SERVICE_MAX_PATH_LEN];
    uint32_t offset;
    uint32_t size;
    uint32_t reserved_size;
    char sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN];
    bool required;
} ota_service_resource_entry_t;

/**
 * @brief 资源索引文件对象。
 */
typedef struct {
    uint32_t format_version;
    char firmware_compat[OTA_SERVICE_MAX_COMPAT_LEN];
    char manifest_version[OTA_SERVICE_MAX_VERSION_LEN];
    size_t resource_count;
    ota_service_resource_entry_t resources[OTA_SERVICE_MAX_RESOURCES];
} ota_service_manifest_t;

/**
 * @brief 资源更新状态文件对象。
 */
typedef struct {
    ota_service_state_t state;
    char target_manifest_version[OTA_SERVICE_MAX_VERSION_LEN];
    char target_resource[OTA_SERVICE_MAX_NAME_LEN];
    char target_partition[OTA_SERVICE_MAX_PARTITION_LEN];
    char target_sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN];
    uint32_t retry_count;
} ota_service_update_state_t;

/**
 * @brief 组件初始化配置。
 */
typedef struct {
    const char *assets_partition_label;
    const char *assets_meta_partition_label;
    const char *assets_base_path;
    const char *assets_meta_base_path;
    const char *metadata_url;
    const char *server_cert_pem;
    uint32_t http_timeout_ms;
    uint32_t worker_stack_size;
    uint32_t worker_priority;
    uint32_t download_buffer_size;
    bool skip_cert_common_name_check;
    bool auto_reboot;
    bool format_if_mount_failed;
} ota_service_config_t;

/**
 * @brief 固件 OTA 元信息。
 */
typedef struct {
    char version[OTA_SERVICE_MAX_VERSION_LEN];
    char url[OTA_SERVICE_MAX_URL_LEN];
    char sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN];
    char description[OTA_SERVICE_MAX_DESC_LEN];
    uint32_t size;
    bool mandatory;
    bool valid;
} ota_service_firmware_info_t;

/**
 * @brief 远端 OTA 元数据。
 *
 * 一个 OTA 地址返回一个总 JSON，其中包含：
 * - `firmware`：固件更新信息
 * - `manifest`：资源索引信息
 */
typedef struct {
    uint32_t protocol_version;
    bool has_firmware;
    bool has_manifest;
    ota_service_firmware_info_t firmware;
    ota_service_manifest_t manifest;
} ota_service_remote_metadata_t;

/**
 * @brief 固件更新计划。
 */
typedef struct {
    bool available;
    bool needs_update;
    bool mandatory;
    char current_version[OTA_SERVICE_MAX_VERSION_LEN];
    char target_version[OTA_SERVICE_MAX_VERSION_LEN];
    char url[OTA_SERVICE_MAX_URL_LEN];
    char sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN];
} ota_service_firmware_plan_t;

/**
 * @brief 待安装的本地资源项。
 */
typedef struct {
    const char *name;
    const char *type;
    const char *version;
    const char *partition;
    ota_service_resource_access_t access;
    const char *path;
    uint32_t offset;
    uint32_t reserved_size;
    const void *data;
    size_t size;
    bool required;
} ota_service_local_resource_t;

/**
 * @brief 待安装的本地资源包描述。
 */
typedef struct {
    const char *firmware_compat;
    const char *manifest_version;
    size_t resource_count;
    const ota_service_local_resource_t *resources;
} ota_service_local_package_t;

/**
 * @brief 单资源更新计划项。
 */
typedef struct {
    char name[OTA_SERVICE_MAX_NAME_LEN];
    char current_version[OTA_SERVICE_MAX_VERSION_LEN];
    char target_version[OTA_SERVICE_MAX_VERSION_LEN];
    bool needs_update;
} ota_service_plan_item_t;

/**
 * @brief 本轮更新计划。
 */
typedef struct {
    bool manifest_changed;
    bool reboot_required;
    bool remote_metadata_valid;
    ota_service_firmware_plan_t firmware;
    size_t resource_count;
    ota_service_plan_item_t items[OTA_SERVICE_MAX_RESOURCES];
} ota_service_update_plan_t;

/**
 * @brief 对外暴露的 OTA 运行状态。
 */
typedef struct {
    ota_service_runtime_state_t runtime_state;
    ota_service_state_t transaction_state;
    bool busy;
    bool need_ota;
    bool firmware_needs_update;
    bool assets_need_update;
    bool completed;
    bool success;
    bool reboot_pending;
    esp_err_t last_error;
    uint32_t total_steps;
    uint32_t completed_steps;
    uint32_t current_file_size;
    uint32_t current_file_downloaded;
    uint32_t current_speed_bytes_per_sec;
    char current_target[OTA_SERVICE_MAX_NAME_LEN];
    char current_version[OTA_SERVICE_MAX_VERSION_LEN];
    char target_version[OTA_SERVICE_MAX_VERSION_LEN];
    ota_service_update_plan_t plan;
} ota_service_status_t;

/**
 * @brief 获取默认配置。
 *
 * @return ota_service_config_t 默认配置对象。
 */
ota_service_config_t ota_service_get_default_config(void);

/**
 * @brief 初始化组件并挂载 `assets` / `assets_meta` LittleFS。
 *
 * @param config 初始化配置，允许传入 NULL 使用默认值。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_init(const ota_service_config_t *config);

/**
 * @brief 启动后台 OTA 流程。
 *
 * 主函数只需提供配置并调用本接口，组件会在内部完成：
 * - 挂载分区
 * - 拉取 OTA 元数据
 * - 判断固件与资源是否需要更新
 * - 自动执行固件 OTA 与资源 OTA
 *
 * @param config 初始化配置，允许传入 NULL 使用默认值。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_start(const ota_service_config_t *config);

/**
 * @brief 反初始化组件并卸载文件系统。
 *
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_deinit(void);

/**
 * @brief 准备默认的 `manifest.json` 和 `state.json`。
 *
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_prepare_storage(void);

/**
 * @brief 读取当前资源索引文件。
 *
 * @param manifest 输出索引对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_load_manifest(ota_service_manifest_t *manifest);

/**
 * @brief 读取当前资源状态文件。
 *
 * @param state 输出状态对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_load_state(ota_service_update_state_t *state);

/**
 * @brief 基于本地资源包构建更新计划。
 *
 * 版本比较使用 `version_checker` 组件，要求版本号为 3 位或 4 位纯数字格式。
 *
 * @param package 本地资源包描述。
 * @param plan 输出更新计划。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_build_update_plan(const ota_service_local_package_t *package,
                                        ota_service_update_plan_t *plan);

/**
 * @brief 将本地资源包写入目标分区并更新 `manifest.json` / `state.json`。
 *
 * @param package 本地资源包描述。
 * @param plan_out 可选输出更新计划，可为 NULL。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_apply_local_package(const ota_service_local_package_t *package,
                                          ota_service_update_plan_t *plan_out);

/**
 * @brief 解析 OTA 地址返回的总 JSON。
 *
 * 自定义协议格式示例：
 * {
 *   "protocol_version": 1,
 *   "firmware": { ... },
 *   "manifest": { ... }
 * }
 *
 * @param json_text 远端返回的 JSON 文本。
 * @param metadata 输出远端元数据。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_parse_remote_metadata(const char *json_text,
                                            ota_service_remote_metadata_t *metadata);

/**
 * @brief 访问配置中的 `metadata_url` 并拉取 OTA 元数据。
 *
 * @param metadata 输出远端元数据。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_fetch_remote_metadata(ota_service_remote_metadata_t *metadata);

/**
 * @brief 基于远端 OTA 元数据构建更新计划。
 *
 * 固件部分使用当前 `app_desc->version` 和远端 `firmware.version` 比较；
 * 资源部分使用当前本地 `manifest.json` 与远端 `manifest` 对比。
 *
 * @param metadata 远端元数据。
 * @param plan 输出更新计划。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_build_remote_update_plan(const ota_service_remote_metadata_t *metadata,
                                               ota_service_update_plan_t *plan);

/**
 * @brief 从 `metadata_url` 拉取远端元数据并直接构建更新计划。
 *
 * @param metadata_out 可选输出远端元数据，可为 NULL。
 * @param plan 输出更新计划。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_check_remote_update(ota_service_remote_metadata_t *metadata_out,
                                          ota_service_update_plan_t *plan);

/**
 * @brief 获取当前 OTA 运行状态。
 *
 * @param status 输出状态对象。
 * @return esp_err_t 执行结果。
 */
esp_err_t ota_service_get_status(ota_service_status_t *status);

/**
 * @brief 判断 OTA 是否已经结束。
 *
 * @return true 已结束。
 * @return false 仍在执行中。
 */
bool ota_service_is_finished(void);

/**
 * @brief 判断当前是否需要 OTA。
 *
 * @return true 需要 OTA。
 * @return false 不需要 OTA 或尚未完成检查。
 */
bool ota_service_need_ota(void);

/**
 * @brief 获取 `assets` 挂载路径。
 *
 * @return const char* 挂载路径。
 */
const char *ota_service_get_assets_base_path(void);

/**
 * @brief 获取 `assets_meta` 挂载路径。
 *
 * @return const char* 挂载路径。
 */
const char *ota_service_get_assets_meta_base_path(void);

/**
 * @brief 将状态枚举转换为字符串。
 *
 * @param state 状态枚举。
 * @return const char* 状态字符串。
 */
const char *ota_service_state_to_string(ota_service_state_t state);

#ifdef __cplusplus
}
#endif
