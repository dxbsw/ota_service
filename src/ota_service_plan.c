#include "ota_service_internal.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"

static esp_err_t ota_service_refresh_models_integrity_from_manifest(const ota_service_manifest_t *manifest)
{
    ota_service_integrity_info_t integrity;
    esp_err_t err = ota_service_load_integrity(&integrity);

    if (err != ESP_OK) {
        ota_service_integrity_set_defaults(&integrity);
    }

    err = ota_service_build_models_integrity(manifest, &integrity.models);
    if (err != ESP_OK) {
        return err;
    }

    return ota_service_save_integrity(&integrity);
}

/**
 * @brief 记录资源写入过程中的事务状态。
 *
 * @param package 本地资源包。
 * @param resource 当前资源。
 * @param state_value 要写入的状态值。
 * @param retry_count 重试次数。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_write_state_progress(const ota_service_local_package_t *package,
                                                  const ota_service_local_resource_t *resource,
                                                  ota_service_state_t state_value,
                                                  uint32_t retry_count)
{
    ota_service_update_state_t state;

    ota_service_state_set_defaults(&state);
    state.state = state_value;
    state.retry_count = retry_count;
    ota_service_copy_string(state.target_manifest_version,
                            sizeof(state.target_manifest_version),
                            package != NULL ? package->manifest_version : NULL);
    ota_service_copy_string(state.target_resource,
                            sizeof(state.target_resource),
                            resource != NULL ? resource->name : NULL);
    ota_service_copy_string(state.target_partition,
                            sizeof(state.target_partition),
                            resource != NULL ? resource->partition : NULL);

    if (resource != NULL && resource->data != NULL && resource->size > 0) {
        ota_service_compute_memory_sha256(resource->data, resource->size, state.target_sha256);
    }

    return ota_service_save_state(&state);
}

/**
 * @brief 将资源文件写入 LittleFS 分区。
 *
 * @param resource 资源对象。
 * @param sha256_hex 输出实际写入后的哈希。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_write_littlefs_resource(const ota_service_local_resource_t *resource,
                                                     char *sha256_hex)
{
    char file_path[256] = {0};
    const char *base_path = ota_service_get_base_path_for_partition(resource->partition);
    esp_err_t err = ESP_OK;

    if (base_path == NULL || resource->path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ota_service_build_partition_path(file_path, sizeof(file_path), base_path, resource->path);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_write_file_atomic(file_path, resource->data, resource->size);
    if (err != ESP_OK) {
        return err;
    }

    return ota_service_compute_file_sha256(file_path, sha256_hex);
}

/**
 * @brief 将资源写入原始分区。
 *
 * @param resource 资源对象。
 * @param sha256_hex 输出实际写入后的哈希。
 * @return esp_err_t 执行结果。
 */
static esp_err_t ota_service_write_mmap_resource(const ota_service_local_resource_t *resource, char *sha256_hex)
{
    const esp_partition_t *partition = NULL;
    size_t erase_size = 0;
    esp_err_t err = ESP_OK;

    partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, resource->partition);
    if (partition == NULL) {
        ESP_LOGE(OTA_SERVICE_TAG, "partition not found: %s", resource->partition);
        return ESP_ERR_NOT_FOUND;
    }

    erase_size = resource->size;
    if ((erase_size % OTA_SERVICE_ERASE_ALIGN) != 0) {
        erase_size += OTA_SERVICE_ERASE_ALIGN - (erase_size % OTA_SERVICE_ERASE_ALIGN);
    }

    err = esp_partition_erase_range(partition, resource->offset, erase_size);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_partition_write(partition, resource->offset, resource->data, resource->size);
    if (err != ESP_OK) {
        return err;
    }

    return ota_service_compute_partition_sha256(partition, resource->offset, resource->size, sha256_hex);
}

/**
 * @brief 将本地资源对象转为清单条目。
 *
 * @param entry 输出清单条目。
 * @param resource 本地资源对象。
 * @param sha256_hex 资源哈希。
 */
static void ota_service_fill_manifest_entry(ota_service_resource_entry_t *entry,
                                            const ota_service_local_resource_t *resource,
                                            const char *sha256_hex)
{
    memset(entry, 0, sizeof(*entry));
    ota_service_copy_string(entry->name, sizeof(entry->name), resource->name);
    ota_service_copy_string(entry->type, sizeof(entry->type), resource->type);
    ota_service_copy_string(entry->version, sizeof(entry->version), resource->version);
    ota_service_copy_string(entry->partition, sizeof(entry->partition), resource->partition);
    entry->access = resource->access;
    ota_service_copy_string(entry->path, sizeof(entry->path), resource->path);
    entry->offset = resource->offset;
    entry->size = (uint32_t)resource->size;
    entry->reserved_size = resource->reserved_size;
    ota_service_copy_string(entry->sha256, sizeof(entry->sha256), sha256_hex);
    entry->required = resource->required;
}

esp_err_t ota_service_build_update_plan(const ota_service_local_package_t *package,
                                        ota_service_update_plan_t *plan)
{
    ota_service_manifest_t current_manifest;
    esp_err_t err = ESP_OK;
    bool any_update = false;

    if (!g_ota_service_ctx.initialized || package == NULL || plan == NULL || package->resources == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(plan, 0, sizeof(*plan));
    err = ota_service_load_manifest(&current_manifest);
    if (err == ESP_ERR_NOT_FOUND) {
        ota_service_manifest_set_defaults(&current_manifest);
    } else if (err != ESP_OK) {
        return err;
    }

    if (package->resource_count > OTA_SERVICE_MAX_RESOURCES) {
        return ESP_ERR_INVALID_SIZE;
    }

    plan->resource_count = package->resource_count;

    for (size_t index = 0; index < package->resource_count; index++) {
        const ota_service_local_resource_t *resource = &package->resources[index];
        ota_service_plan_item_t *item = &plan->items[index];
        int current_index = -1;

        err = ota_service_validate_resource(resource);
        if (err != ESP_OK) {
            return err;
        }

        ota_service_copy_string(item->name, sizeof(item->name), resource->name);
        ota_service_copy_string(item->target_version, sizeof(item->target_version), resource->version);

        current_index = ota_service_find_manifest_resource_index(&current_manifest, resource->name);
        if (current_index < 0) {
            item->needs_update = true;
            any_update = true;
            continue;
        }

        ota_service_copy_string(item->current_version,
                                sizeof(item->current_version),
                                current_manifest.resources[current_index].version);

        if (ota_service_version_is_equal_safe(item->current_version, item->target_version)) {
            const ota_service_resource_entry_t *current_entry = &current_manifest.resources[current_index];
            bool metadata_changed = current_entry->access != resource->access ||
                                    current_entry->size != resource->size ||
                                    strcmp(current_entry->partition, resource->partition) != 0;

            if (!metadata_changed && resource->access == OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS && resource->path != NULL) {
                metadata_changed = strcmp(current_entry->path, resource->path) != 0;
            }

            if (!metadata_changed && resource->access == OTA_SERVICE_RESOURCE_ACCESS_MMAP) {
                metadata_changed = current_entry->offset != resource->offset ||
                                   current_entry->reserved_size != resource->reserved_size;
            }

            item->needs_update = metadata_changed;
        } else if (ota_service_version_is_target_newer(item->current_version, item->target_version)) {
            item->needs_update = true;
        } else {
            item->needs_update = false;
            ESP_LOGW(OTA_SERVICE_TAG,
                     "skip downgrade or invalid version compare, resource=%s current=%s target=%s",
                     resource->name,
                     item->current_version,
                     item->target_version);
        }

        if (item->needs_update) {
            any_update = true;
        }
    }

    if (current_manifest.manifest_version[0] == 0) {
        plan->manifest_changed = true;
    } else if (package->manifest_version != NULL && package->manifest_version[0] != 0) {
        if (!ota_service_version_is_equal_safe(current_manifest.manifest_version, package->manifest_version) &&
            ota_service_version_is_target_newer(current_manifest.manifest_version, package->manifest_version)) {
            plan->manifest_changed = true;
        }
    }

    if (any_update) {
        plan->manifest_changed = true;
        plan->reboot_required = true;
    }

    return ESP_OK;
}

esp_err_t ota_service_apply_local_package(const ota_service_local_package_t *package,
                                          ota_service_update_plan_t *plan_out)
{
    ota_service_manifest_t next_manifest;
    ota_service_update_plan_t plan;
    ota_service_update_state_t current_state;
    esp_err_t err = ESP_OK;
    bool state_loaded = false;

    if (!g_ota_service_ctx.initialized || package == NULL || package->resources == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = ota_service_prepare_storage();
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_build_update_plan(package, &plan);
    if (err != ESP_OK) {
        return err;
    }

    err = ota_service_load_state(&current_state);
    if (err == ESP_OK) {
        state_loaded = true;
    } else {
        ota_service_state_set_defaults(&current_state);
    }

    err = ota_service_load_manifest(&next_manifest);
    if (err == ESP_ERR_NOT_FOUND) {
        ota_service_manifest_set_defaults(&next_manifest);
    } else if (err != ESP_OK) {
        return err;
    }

    next_manifest.format_version = 2;
    ota_service_copy_string(next_manifest.firmware_compat,
                            sizeof(next_manifest.firmware_compat),
                            package->firmware_compat);
    ota_service_copy_string(next_manifest.manifest_version,
                            sizeof(next_manifest.manifest_version),
                            package->manifest_version);

    for (size_t index = 0; index < package->resource_count; index++) {
        const ota_service_local_resource_t *resource = &package->resources[index];
        ota_service_plan_item_t *plan_item = &plan.items[index];
        int manifest_index = ota_service_find_manifest_resource_index(&next_manifest, resource->name);
        ota_service_resource_entry_t new_entry;
        char expected_sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN] = {0};
        char actual_sha256[OTA_SERVICE_MAX_SHA256_HEX_LEN] = {0};

        ota_service_compute_memory_sha256(resource->data, resource->size, expected_sha256);

        if (!plan_item->needs_update) {
            continue;
        }

        err = ota_service_write_state_progress(package,
                                               resource,
                                               OTA_SERVICE_STATE_DOWNLOADING,
                                               current_state.retry_count);
        if (err != ESP_OK) {
            goto failed;
        }

        err = ota_service_write_state_progress(package,
                                               resource,
                                               OTA_SERVICE_STATE_WRITING,
                                               current_state.retry_count);
        if (err != ESP_OK) {
            goto failed;
        }

        if (resource->access == OTA_SERVICE_RESOURCE_ACCESS_LITTLEFS) {
            err = ota_service_write_littlefs_resource(resource, actual_sha256);
        } else {
            err = ota_service_write_mmap_resource(resource, actual_sha256);
        }
        if (err != ESP_OK) {
            goto failed;
        }

        err = ota_service_write_state_progress(package,
                                               resource,
                                               OTA_SERVICE_STATE_VERIFYING,
                                               current_state.retry_count);
        if (err != ESP_OK) {
            goto failed;
        }

        if (strcmp(expected_sha256, actual_sha256) != 0) {
            err = ESP_ERR_INVALID_CRC;
            goto failed;
        }

        ota_service_fill_manifest_entry(&new_entry, resource, actual_sha256);

        if (manifest_index >= 0) {
            next_manifest.resources[manifest_index] = new_entry;
        } else {
            if (next_manifest.resource_count >= OTA_SERVICE_MAX_RESOURCES) {
                err = ESP_ERR_INVALID_SIZE;
                goto failed;
            }
            next_manifest.resources[next_manifest.resource_count++] = new_entry;
        }
    }

    if (plan.manifest_changed) {
        err = ota_service_save_manifest(&next_manifest);
        if (err != ESP_OK) {
            goto failed;
        }

        err = ota_service_refresh_models_integrity_from_manifest(&next_manifest);
        if (err != ESP_OK) {
            goto failed;
        }
    }

    ota_service_state_set_defaults(&current_state);
    err = ota_service_save_state(&current_state);
    if (err != ESP_OK) {
        return err;
    }

    if (plan_out != NULL) {
        *plan_out = plan;
    }

    return ESP_OK;

failed:
    current_state.state = OTA_SERVICE_STATE_FAILED;
    current_state.retry_count = state_loaded ? current_state.retry_count + 1 : 1;
    if (package->manifest_version != NULL) {
        ota_service_copy_string(current_state.target_manifest_version,
                                sizeof(current_state.target_manifest_version),
                                package->manifest_version);
    }
    ota_service_save_state(&current_state);
    return err;
}

esp_err_t ota_service_build_remote_update_plan(const ota_service_remote_metadata_t *metadata,
                                               ota_service_update_plan_t *plan)
{
    ota_service_manifest_t current_manifest;
    ota_service_integrity_info_t integrity;
    ota_service_partition_integrity_t actual_models_integrity;
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *effective_firmware_version = NULL;
    esp_err_t err = ESP_OK;
    bool any_update = false;
    bool has_local_models = false;
    bool models_integrity_ok = true;

    if (!g_ota_service_ctx.initialized || metadata == NULL || plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(plan, 0, sizeof(*plan));
    plan->remote_metadata_valid = true;

    if (metadata->has_firmware && metadata->firmware.valid) {
        plan->firmware.available = true;
        plan->firmware.mandatory = metadata->firmware.mandatory;
        ota_service_copy_string(plan->firmware.current_version,
                                sizeof(plan->firmware.current_version),
                                app_desc != NULL ? app_desc->version : NULL);
        ota_service_copy_string(plan->firmware.target_version,
                                sizeof(plan->firmware.target_version),
                                metadata->firmware.version);
        ota_service_copy_string(plan->firmware.url,
                                sizeof(plan->firmware.url),
                                metadata->firmware.url);
        ota_service_copy_string(plan->firmware.sha256,
                                sizeof(plan->firmware.sha256),
                                metadata->firmware.sha256);

        if (plan->firmware.current_version[0] == 0) {
            plan->firmware.needs_update = true;
        } else if (ota_service_version_is_target_newer(plan->firmware.current_version,
                                                       plan->firmware.target_version)) {
            plan->firmware.needs_update = true;
        }

        if (plan->firmware.needs_update) {
            any_update = true;
            plan->reboot_required = true;
        }
    }

    if (!metadata->has_manifest) {
        return ESP_OK;
    }

    effective_firmware_version = plan->firmware.needs_update ?
                                     plan->firmware.target_version :
                                     plan->firmware.current_version;
    if (metadata->manifest.firmware_compat[0] != 0 &&
        !ota_service_version_is_at_least(effective_firmware_version, metadata->manifest.firmware_compat)) {
        ESP_LOGE(OTA_SERVICE_TAG,
                 "manifest firmware_compat not satisfied, effective=%s required=%s",
                 effective_firmware_version != NULL && effective_firmware_version[0] != 0 ?
                     effective_firmware_version :
                     "<none>",
                 metadata->manifest.firmware_compat);
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = ota_service_load_manifest(&current_manifest);
    if (err == ESP_ERR_NOT_FOUND) {
        ota_service_manifest_set_defaults(&current_manifest);
    } else if (err != ESP_OK) {
        return err;
    }

    if (metadata->manifest.resource_count > OTA_SERVICE_MAX_RESOURCES) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t index = 0; index < current_manifest.resource_count; index++) {
        if (ota_service_resource_entry_is_models(&current_manifest.resources[index])) {
            has_local_models = true;
            break;
        }
    }

    if (has_local_models) {
        err = ota_service_load_integrity(&integrity);
        if (err != ESP_OK) {
            ota_service_integrity_set_defaults(&integrity);
        }

        err = ota_service_build_models_integrity(&current_manifest, &actual_models_integrity);
        if (err != ESP_OK ||
            integrity.models.sha256[0] == 0 ||
            strcmp(integrity.models.sha256, actual_models_integrity.sha256) != 0) {
            models_integrity_ok = false;
            ESP_LOGW(OTA_SERVICE_TAG, "models integrity invalid, target resources will be forced to ota");
        }
    }

    plan->resource_count = metadata->manifest.resource_count;

    for (size_t index = 0; index < metadata->manifest.resource_count; index++) {
        const ota_service_resource_entry_t *target_entry = &metadata->manifest.resources[index];
        ota_service_plan_item_t *item = &plan->items[index];
        int current_index = ota_service_find_manifest_resource_index(&current_manifest, target_entry->name);

        ota_service_copy_string(item->name, sizeof(item->name), target_entry->name);
        ota_service_copy_string(item->target_version, sizeof(item->target_version), target_entry->version);

        if (current_index < 0) {
            item->needs_update = true;
            any_update = true;
            continue;
        }

        ota_service_copy_string(item->current_version,
                                sizeof(item->current_version),
                                current_manifest.resources[current_index].version);

        if (!models_integrity_ok && ota_service_resource_entry_is_models(target_entry)) {
            item->needs_update = true;
        } else if (ota_service_version_is_equal_safe(item->current_version, item->target_version)) {
            item->needs_update = ota_service_resource_entry_metadata_changed(&current_manifest.resources[current_index],
                                                                             target_entry);
        } else if (ota_service_version_is_target_newer(item->current_version, item->target_version)) {
            item->needs_update = true;
        } else {
            item->needs_update = false;
            ESP_LOGW(OTA_SERVICE_TAG,
                     "skip remote downgrade or invalid version compare, resource=%s current=%s target=%s",
                     target_entry->name,
                     item->current_version,
                     item->target_version);
        }

        if (item->needs_update) {
            any_update = true;
        }
    }

    if (current_manifest.manifest_version[0] == 0) {
        plan->manifest_changed = true;
    } else if (metadata->manifest.manifest_version[0] != 0) {
        if (!ota_service_version_is_equal_safe(current_manifest.manifest_version,
                                               metadata->manifest.manifest_version) &&
            ota_service_version_is_target_newer(current_manifest.manifest_version,
                                                metadata->manifest.manifest_version)) {
            plan->manifest_changed = true;
        }
    }

    if (any_update) {
        plan->manifest_changed = true;
        plan->reboot_required = true;
    }

    return ESP_OK;
}
