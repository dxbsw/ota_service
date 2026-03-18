idf_component_register(SRCS "src/ota_service.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_https_ota esp_http_client json esp_timer app_update nvs_flash)
