# OTA Service

`ota_service` 是一个面向 ESP-IDF 的“固件 + 资源”一体化 OTA 组件。

它的目标是让主程序只保留最少配置和一个启动入口，组件内部自动完成：

- 挂载 `assets` / `assets_meta`
- 读取和维护本地 `manifest.json` / `state.json` / `integrity.json`
- 访问统一 `metadata_url`
- 判断固件与资源是否需要更新
- 自动执行固件 OTA
- 自动执行资源逐文件 OTA
- 启动时校验 `models` 完整性并在异常时强制重新 OTA
- 对外提供运行状态、进度和下载速度查询

当前工程已经完成 OTA 联调验证，支持“固件更新 + LittleFS 资源更新 + 本地初始资源基线 + Python 测试服务端”这一整套闭环。

作为独立组件使用时，建议优先阅读：

- `使用说明.md`
- `OTA开发文档.md`

## 核心能力

- 统一 OTA 地址：设备只访问一个 `metadata_url`
- 固件 OTA：按远端 `firmware.version` 与本地固件版本比较
- 资源 OTA：按本地 `manifest.json` 与远端 `manifest` 比较
- 增量更新：仅下载有差异的资源
- 文件校验：固件和资源都进行 `sha256` 校验
- 事务状态：通过 `state.json` 记录 `idle/downloading/writing/verifying/failed`
- 完整性记录：通过 `integrity.json` 维护固件和 `models` 校验信息
- 运行态查询：支持查看阶段、目标、步数、文件进度、速度、错误码
- PSRAM 优先：下载缓冲优先申请 PSRAM，失败后回退内部 RAM

## 目录结构

```text
ota_service/
├── include/
│   └── ota_service.h
├── src/
│   ├── ota_service_core.c
│   ├── ota_service_internal.h
│   ├── ota_service_plan.c
│   ├── ota_service_remote.c
│   ├── ota_service_storage.c
│   └── ota_service_worker.c
├── python_tool/
│   ├── ota_server.py
│   ├── ota_server_config.json
│   └── server_data/
├── CMakeLists.txt
├── idf_component.yml
├── README.md
├── 使用说明.md
└── OTA开发文档.md
```

模块职责：

- `ota_service_core.c`：默认配置、挂载/卸载、全局状态管理
- `ota_service_storage.c`：`manifest.json` / `state.json` / `integrity.json` 读写、文件与分区哈希
- `ota_service_plan.c`：本地包/远端清单的更新计划构建
- `ota_service_remote.c`：HTTP 拉取与总 JSON 解析
- `ota_service_worker.c`：后台 OTA task、固件下载写入、资源逐文件更新
- `python_tool/ota_server.py`：本地联调用 Python 服务端

## 依赖与约束

- `joltwallet/littlefs:^1.21.1`
- `dxbsw/version_checker:^1.0.0`
- `json`
- `app_update`
- `esp_http_client`
- `esp_timer`
- `mbedtls`

版本比较使用 `dxbsw/version_checker`，因此版本号建议统一使用：

- `1.0.0`
- `1.0.0.1`

不建议使用带字母或后缀的版本号。

## 分区要求

推荐分区如下：

```csv
ota_0,       app,  ota_0,      0x10000,  0x400000,
ota_1,       app,  ota_1,      0x410000, 0x400000,
models,      data, ,           0x810000, 0x200000,
assets,      data, littlefs,   0xA10000, 0x5A3000,
assets_meta, data, littlefs,   0xFB3000, 0xA000,
```

说明：

- `ota_0` / `ota_1`：固件双分区
- `models`：原始分区资源，可用于 `mmap` 模型文件
- `assets`：LittleFS 资源分区
- `assets_meta`：LittleFS 控制面分区，保存 `manifest.json` / `state.json` / `integrity.json`

## 快速接入

头文件：

```c
#include "ota_service.h"
```

推荐主流程：

```c
ota_service_config_t config = ota_service_get_default_config();
ota_service_status_t status = {0};

config.metadata_url = "http://192.168.3.198:8000/ota/metadata";
config.auto_reboot = true;

ESP_ERROR_CHECK(ota_service_start(&config));

while (!ota_service_is_finished()) {
    if (ota_service_get_status(&status) == ESP_OK) {
        printf("runtime=%d target=%s step=%u/%u speed=%uB/s\n",
               (int)status.runtime_state,
               status.current_target,
               (unsigned int)status.completed_steps,
               (unsigned int)status.total_steps,
               (unsigned int)status.current_speed_bytes_per_sec);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}
```

推荐只使用以下接口：

- `ota_service_get_default_config()`
- `ota_service_start()`
- `ota_service_get_status()`
- `ota_service_is_finished()`
- `ota_service_need_ota()`

调试或高级场景可使用：

- `ota_service_init()`
- `ota_service_load_manifest()`
- `ota_service_load_state()`
- `ota_service_apply_local_package()`
- `ota_service_check_remote_update()`

## 默认配置

默认配置由 `src/ota_service_core.c` 提供，当前关键默认值为：

- `assets_partition_label = "assets"`
- `assets_meta_partition_label = "assets_meta"`
- `assets_base_path = "/assets"`
- `assets_meta_base_path = "/assets_meta"`
- `http_timeout_ms = 5000`
- `worker_stack_size = 24576`
- `worker_priority = 5`
- `download_buffer_size = 8192`
- `auto_reboot = false`
- `format_if_mount_failed = true`

## OTA 协议

组件通过 `metadata_url` 访问统一 OTA 地址，服务端返回一个总 JSON：

```json
{
  "protocol_version": 1,
  "firmware": {
    "version": "1.0.2",
    "url": "http://192.168.3.198:8000/firmware/BLE_TEST.bin",
    "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "size": 1048576,
    "mandatory": false,
    "description": "BLE_TEST firmware 1.0.2 test release"
  },
  "manifest": {
    "format_version": 2,
    "firmware_compat": "1.0.1",
    "manifest_version": "1.0.1",
    "resources": [
      {
        "name": "test_text",
        "type": "text",
        "version": "1.0.2",
        "partition": "assets",
        "access": "littlefs",
        "path": "/texts/test_0.txt",
        "size": 3194,
        "sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "required": true
      }
    ]
  }
}
```

字段说明：

- `protocol_version`：协议版本，当前使用 `1`
- `firmware`：固件更新对象，可省略
- `manifest`：资源清单对象，可省略
- `manifest.resources[].access`：
  - `littlefs`：写入 LittleFS
  - `mmap`：写入原始分区

资源文件下载地址由组件内部自动拼接：

- `assets` 资源：`http://<host>/assets{path}`
- `models` 资源：`http://<host>/models{path}`

## 更新规则

固件判断规则：

- 远端 `firmware.version` 大于当前固件版本时执行固件 OTA
- 固件写入使用 `esp_ota_begin()` / `esp_ota_write()` / `esp_ota_end()`
- 固件写入完成后会再次校验目标 OTA 分区中的实际镜像
- 只有整轮 OTA 和完整性校验都成功后，才会设置启动分区
- 若 `auto_reboot = true`，组件会在整轮 OTA 完成后自动重启

资源判断规则：

- 本地不存在该资源：更新
- 目标版本大于当前版本：更新
- 版本相同但元数据变化：更新
- `models` 本地完整性校验失败：强制更新
- `manifest.firmware_compat` 高于当前固件版本：跳过资源更新并报错
- 目标版本小于当前版本：跳过，不执行降级

资源执行规则：

- 先比较出需要更新的资源列表
- 逐个下载目标文件
- 每个文件执行“下载 -> 写入 -> 校验 -> 更新本地清单”
- 前一个资源成功后再处理下一个资源
- OTA 过程中最多自动重试 `3` 次，超过后返回失败

## 运行状态

`ota_service_get_status()` 返回 `ota_service_status_t`，常用字段如下：

- `runtime_state`：运行阶段
- `transaction_state`：当前事务状态
- `busy`：组件是否仍在工作
- `need_ota`：本轮是否判定需要 OTA
- `success`：本轮是否成功结束
- `reboot_pending`：是否等待重启
- `completed_steps` / `total_steps`：总进度
- `current_target`：当前目标，如 `firmware` 或资源名
- `current_file_size` / `current_file_downloaded`：当前文件进度
- `current_speed_bytes_per_sec`：当前下载速度
- `last_error`：最近错误码
- `plan`：本轮更新计划详情

## 本地控制文件

组件会在 `assets_meta` 维护三个文件：

- `manifest.json`：本地资源清单
- `state.json`：事务状态文件
- `integrity.json`：固件与 `models` 分区完整性记录

`manifest.json` 保存：

- 资源名
- 类型
- 版本
- 分区
- 路径或偏移
- 大小
- `sha256`
- 是否必需

`state.json` 保存：

- 当前事务状态
- 目标清单版本
- 目标资源名
- 目标分区
- 目标资源哈希
- 重试次数

`integrity.json` 保存：

- `firmware`：最近一次通过校验的 OTA 固件版本、大小和分区哈希
- `models`：本地 `models` 资源聚合校验值

用途：

- 固件写入后先校验目标 OTA 分区，再允许切换启动分区
- 启动时重新校验本地 `models`，若校验失败则强制对应资源重新 OTA
- 三个控制文件缺失或损坏时，会自动按默认值重建，其中默认版本为 `0.0.0`

## Python 测试服务端

测试服务端位于 `python_tool/ota_server.py`，目录结构如下：

```text
python_tool/
├── ota_server.py
├── ota_server_config.json
└── server_data/
    ├── firmware/
    ├── models/
    ├── assets/
    └── generated/
```

说明：

- `server_data/firmware`：固件文件
- `server_data/models`：模型资源
- `server_data/assets`：LittleFS 资源文件
- `server_data/generated`：启动时动态生成的 `manifest.json` / `metadata.json` / `version.json`
- `ota_server_config.json`：固件版本、模型版本、资源版本和文件映射配置

服务端启动时会自动：

- 读取 `ota_server_config.json`
- 校验并统计固件与资源文件
- 计算文件 `size`
- 计算文件 `sha256`
- 生成 `manifest.json`
- 生成 `metadata.json`
- 生成 `version.json`

如果配置了 `firmware.sync_from_build = true`，还会把工程 `build/BLE_TEST.bin` 自动同步到 `server_data/firmware/BLE_TEST.bin`。

## 独立组件接入建议

如果你要把 `ota_service` 单独集成到其他工程，推荐阅读顺序：

1. `使用说明.md`
2. `include/ota_service.h`
3. `OTA开发文档.md`

其中：

- `使用说明.md` 负责讲“怎么接入、怎么配、怎么启动”
- `include/ota_service.h` 负责讲“对外 API 和结构体”
- `OTA开发文档.md` 负责讲“内部实现、流程和机制”

当前主工程 `main/main.c` 仍然保留了一套完整联调示例，可作为参考，但组件本身已经具备独立文档，不再依赖主工程源码才能理解如何使用。

## 配置语义

- 第一次调用 `ota_service_start(config)` 时，会完成初始化并保存当前配置
- 如果组件已经初始化且后台 worker 未运行，再次调用 `ota_service_start(config)` 时，会更新运行配置并启动新一轮 OTA
- 允许在重新启动前更新的字段包括：`metadata_url`、`server_cert_pem`、`http_timeout_ms`、`worker_stack_size`、`worker_priority`、`download_buffer_size`、`skip_cert_common_name_check`、`auto_reboot`
- `assets_partition_label`、`assets_meta_partition_label`、`assets_base_path`、`assets_meta_base_path` 属于挂载相关配置；初始化完成后如果想修改它们，需要先 `ota_service_deinit()` 再重新初始化

## 联调建议

- 首次验证“内置资源基线”时，建议清空设备上的 `assets` 和 `assets_meta`
- 服务端关闭时，设备侧会在状态里看到 `ESP_ERR_HTTP_CONNECT`
- 固件 OTA 成功后若未自动进入新固件，优先检查 `auto_reboot`
- 若只想看比较结果而不自动重启，可设置 `config.auto_reboot = false`

## 补充文档

- 独立接入说明见 `使用说明.md`
- 设计与实现细节见 `OTA开发文档.md`
- 对外 API 定义见 `include/ota_service.h`
