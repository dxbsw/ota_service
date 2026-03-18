#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_SERVICE_EVENT_START_CHECK,
    OTA_SERVICE_EVENT_UPDATE_AVAILABLE,
    OTA_SERVICE_EVENT_NO_UPDATE,
    OTA_SERVICE_EVENT_START_DOWNLOAD,
    OTA_SERVICE_EVENT_DOWNLOADING,
    OTA_SERVICE_EVENT_FINISH,
    OTA_SERVICE_EVENT_ERROR
} ota_service_event_t;

typedef struct {
    char version[32];
    char url[256];
    char description[128];
    bool mandatory;
    uint32_t total_size;
} ota_service_update_info_t;

typedef struct {
    int progress_percent;
    float speed_kbps;
    uint32_t downloaded_bytes;
    uint32_t total_bytes;
} ota_service_download_status_t;

typedef struct {
    ota_service_event_t event;
    union {
        ota_service_update_info_t update_info;
        ota_service_download_status_t download_status;
        const char *error_msg;
    } data;
} ota_service_event_data_t;

typedef void (*ota_service_callback_t)(const ota_service_event_data_t *event_data);

typedef struct {
    const char *check_url;      // URL to version.json
    ota_service_callback_t callback;
} ota_service_config_t;

/**
 * @brief Initialize OTA component
 * 
 * @param config Configuration
 * @return esp_err_t 
 */
esp_err_t ota_service_init(const ota_service_config_t *config);

/**
 * @brief Check connection callback type
 */
typedef bool (*ota_service_is_connected_cb_t)(void);

/**
 * @brief OTA task configuration
 */
typedef struct {
    const char *check_url;                  // URL to version.json
    uint32_t check_interval_ms;             // Interval between checks (ms)
    ota_service_callback_t event_callback;       // Optional: Custom event handler. If NULL, default handler is used.
    ota_service_is_connected_cb_t is_connected_cb; // Optional: Check connectivity before update check
} ota_service_task_config_t;

/**
 * @brief Start OTA background task
 * 
 * @param config Configuration
 * @return esp_err_t 
 */
esp_err_t ota_service_start_task(const ota_service_task_config_t *config);

/**
 * @brief Check for update
 * 
 * @return esp_err_t 
 */
esp_err_t ota_service_check_update(void);

/**
 * @brief Start OTA update (download and flash)
 * 
 * @return esp_err_t 
 */
esp_err_t ota_service_start_update(void);

#ifdef __cplusplus
}
#endif
