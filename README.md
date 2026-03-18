# OTA Service (ESP-IDF)
 
一个基于 ESP-IDF 的 OTA 更新组件，提供版本检查、下载进度上报、下载与刷新、以及任务化后台定时检查等能力。
 
## 功能
 
- 通过 HTTP(S) 获取 `version.json` 并解析更新信息（版本、URL、描述、强制更新、大小）
- 进度回调：百分比、速度（KB/s）、已下载/总字节
- HTTPS OTA 下载与刷新，完成后自动重启
- 后台任务定时检查，支持在联网后再进行检查
 
## 依赖
 
- `esp_https_ota`, `esp_http_client`, `json`, `esp_timer`, `app_update`, `nvs_flash`
 
## 使用示例
 
```c
#include "ota_service.h"
#include "wifi_driver.h" // 示例：你的联网判断
 
#define OTA_CHECK_URL "http://<your-ip>:8000/version.json"
 
void app_main(void)
{
    ota_service_task_config_t ota_config = {
        .check_url = OTA_CHECK_URL,
        .check_interval_ms = 3600000, // 1 hour
        .is_connected_cb = wifi_driver_is_connected
    };
    ota_service_start_task(&ota_config);
}
```
 
## 组件接入（使用组件管理器）
 
在主组件或应用的 `idf_component.yml` 中添加依赖：
 
```yaml
dependencies:
  ota_service:
    git: "https://github.com/dxbsw/ota_service.git"
    version: "v1.0.0"
```
 
随后 `idf.py reconfigure` 或 `idf.py build` 即可自动下载并集成。
 
## 版本
 
- v1.0.0 初始版本
