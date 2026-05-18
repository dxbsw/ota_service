import argparse
import hashlib
import http.server
import json
import shutil
import socketserver
from functools import partial
from pathlib import Path


DEFAULT_IP = "192.168.3.198"
DEFAULT_PORT = 8000
DEFAULT_PROTOCOL_VERSION = 1
DEFAULT_FORMAT_VERSION = 2
VALID_FIRMWARE_MAGIC = 0xE9

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent.parent
PROJECT_CMAKE_PATH = PROJECT_ROOT / "CMakeLists.txt"
CONFIG_PATH = SCRIPT_DIR / "ota_server_config.json"

DATA_ROOT = SCRIPT_DIR / "server_data"
FIRMWARE_ROOT = DATA_ROOT / "firmware"
MODELS_ROOT = DATA_ROOT / "models"
ASSETS_ROOT = DATA_ROOT / "assets"
GENERATED_ROOT = DATA_ROOT / "generated"

LEGACY_ASSETS_ROOT = SCRIPT_DIR / "assets"
LEGACY_FIRMWARE_ROOT = SCRIPT_DIR / "firmware"
MAIN_ASSETS_DIR = PROJECT_ROOT / "main" / "assets"
BUILD_FIRMWARE_PATH = PROJECT_ROOT / "build" / "BLE_TEST.bin"

METADATA_PATH = "/ota/metadata"
MANIFEST_ROUTE = "/manifest.json"
LEGACY_VERSION_ROUTE = "/version.json"


def parse_args():
    """解析启动参数。"""
    parser = argparse.ArgumentParser(description="OTA metadata test server")
    parser.add_argument("ip", nargs="?", default=DEFAULT_IP, help="server ip shown in generated URLs")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="server port")
    parser.add_argument("--config", default=str(CONFIG_PATH), help="server config file path")
    return parser.parse_args()


def read_json(file_path):
    """读取 JSON 文件。"""
    with open(file_path, "r", encoding="utf-8") as file:
        return json.loads(strip_json_comments(file.read()))


def strip_json_comments(text):
    """移除 JSON/JSONC 文本中的注释，保留字符串内容。"""
    result = []
    index = 0
    in_string = False
    escape = False

    while index < len(text):
        current = text[index]
        next_char = text[index + 1] if index + 1 < len(text) else ""

        if in_string:
            result.append(current)
            if escape:
                escape = False
            elif current == "\\":
                escape = True
            elif current == '"':
                in_string = False
            index += 1
            continue

        if current == '"':
            in_string = True
            result.append(current)
            index += 1
            continue

        if current == "/" and next_char == "/":
            index += 2
            while index < len(text) and text[index] not in ("\n", "\r"):
                index += 1
            continue

        if current == "/" and next_char == "*":
            index += 2
            while index + 1 < len(text) and not (text[index] == "*" and text[index + 1] == "/"):
                index += 1
            index += 2
            continue

        result.append(current)
        index += 1

    return "".join(result)


def write_json(file_path, data):
    """将 JSON 持久化到文件，便于调试。"""
    file_path.parent.mkdir(parents=True, exist_ok=True)
    with open(file_path, "w", encoding="utf-8") as file:
        json.dump(data, file, indent=2, ensure_ascii=False)
        file.write("\n")


def sha256_of_file(file_path):
    """计算文件 SHA256。"""
    hasher = hashlib.sha256()
    with open(file_path, "rb") as file:
        while True:
            chunk = file.read(4096)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


def read_project_version():
    """从工程 `CMakeLists.txt` 中读取当前项目版本。"""
    if not PROJECT_CMAKE_PATH.exists():
        return None

    for line in PROJECT_CMAKE_PATH.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("set(PROJECT_VER"):
            continue

        first_quote = line.find('"')
        second_quote = line.find('"', first_quote + 1)
        if first_quote >= 0 and second_quote > first_quote:
            return line[first_quote + 1 : second_quote]

    return None


def ensure_server_layout():
    """创建服务端目录结构。"""
    for directory in (DATA_ROOT, FIRMWARE_ROOT, MODELS_ROOT, ASSETS_ROOT, GENERATED_ROOT):
        directory.mkdir(parents=True, exist_ok=True)


def seed_demo_files():
    """首次运行时补齐示例数据，便于直接联调。"""
    seed_pairs = [
        (BUILD_FIRMWARE_PATH, FIRMWARE_ROOT / "BLE_TEST.bin"),
        (LEGACY_FIRMWARE_ROOT / "BLE_TEST.bin", FIRMWARE_ROOT / "BLE_TEST.bin"),
        (MAIN_ASSETS_DIR / "test_0.txt", ASSETS_ROOT / "texts" / "test_0.txt"),
        (MAIN_ASSETS_DIR / "test_1.json", ASSETS_ROOT / "configs" / "test_1.json"),
        (LEGACY_ASSETS_ROOT / "texts" / "test_0.txt", ASSETS_ROOT / "texts" / "test_0.txt"),
        (LEGACY_ASSETS_ROOT / "configs" / "test_1.json", ASSETS_ROOT / "configs" / "test_1.json"),
    ]

    for source_path, target_path in seed_pairs:
        if target_path.exists() or not source_path.exists():
            continue

        target_path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_path, target_path)


def load_config(config_path):
    """加载 OTA 服务端配置。"""
    config_file = Path(config_path).resolve()
    if not config_file.exists():
        raise FileNotFoundError(f"missing ota server config: {config_file}")

    return read_json(config_file)


def resolve_data_file(relative_path):
    """将相对路径转换为 `server_data` 下的真实路径。"""
    if not relative_path:
        raise ValueError("resource file path is empty")

    file_path = DATA_ROOT / relative_path
    file_path = file_path.resolve()
    data_root_resolved = DATA_ROOT.resolve()
    if data_root_resolved not in file_path.parents and file_path != data_root_resolved:
        raise ValueError(f"path must stay inside server_data: {relative_path}")
    return file_path


def resource_url_prefix(resource):
    """根据资源类型选择静态下载路径前缀。"""
    access = resource.get("access", "littlefs")
    partition = resource.get("partition", "assets")
    if access == "mmap" or partition == "models":
        return "/models"
    return "/assets"


def validate_firmware_image(firmware_path):
    """校验固件文件是否为 ESP-IDF 应用镜像。"""
    with open(firmware_path, "rb") as file:
        first_byte = file.read(1)

    if len(first_byte) != 1:
        raise ValueError(f"firmware file is empty: {firmware_path}")

    if first_byte[0] != VALID_FIRMWARE_MAGIC:
        raise ValueError(
            f"invalid firmware image: {firmware_path}, expected magic 0xE9, got 0x{first_byte[0]:02X}"
        )


def build_firmware_info(config, ip, port):
    """根据配置和真实文件生成固件描述。"""
    firmware_cfg = config.get("firmware", {})
    firmware_file = resolve_data_file(firmware_cfg.get("file", "firmware/BLE_TEST.bin"))
    firmware_version = firmware_cfg.get("version", "")
    project_version = read_project_version()

    if firmware_cfg.get("sync_from_build", False) and BUILD_FIRMWARE_PATH.exists():
        firmware_file.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(BUILD_FIRMWARE_PATH, firmware_file)

    if not firmware_file.exists():
        raise FileNotFoundError(f"missing firmware file: {firmware_file}")

    validate_firmware_image(firmware_file)

    if project_version and firmware_version and project_version != firmware_version:
        print(
            f"Warning: configured firmware version {firmware_version} != project version {project_version} "
            f"from {PROJECT_CMAKE_PATH.name}"
        )

    firmware_name = firmware_file.name
    firmware_url = f"http://{ip}:{port}/firmware/{firmware_name}"
    description = firmware_cfg.get("description") or f"BLE_TEST firmware {firmware_version} test release"

    return {
        "version": firmware_version,
        "url": firmware_url,
        "sha256": sha256_of_file(firmware_file),
        "size": firmware_file.stat().st_size,
        "mandatory": bool(firmware_cfg.get("mandatory", False)),
        "description": description,
    }


def build_resource_entry(resource, default_partition, default_access):
    """生成单个资源清单项。"""
    relative_file = resource.get("file")
    actual_file = resolve_data_file(relative_file)
    manifest_path = resource.get("path")
    partition = resource.get("partition", default_partition)
    access = resource.get("access", default_access)

    if not actual_file.exists():
        raise FileNotFoundError(f"missing resource file: {actual_file}")
    if not manifest_path:
        raise ValueError(f"resource path is empty for {resource.get('name', relative_file)}")

    entry = {
        "name": resource["name"],
        "type": resource["type"],
        "version": resource["version"],
        "partition": partition,
        "access": access,
        "path": manifest_path,
        "size": actual_file.stat().st_size,
        "sha256": sha256_of_file(actual_file),
        "required": bool(resource.get("required", False)),
    }

    if access == "mmap" or partition == "models":
        entry["offset"] = int(resource.get("offset", 0))
        entry["reserved_size"] = int(resource.get("reserved_size", entry["size"]))

    return entry


def build_manifest(config):
    """生成资源 manifest。"""
    resources = []
    for model in config.get("models", []):
        resources.append(build_resource_entry(model, "models", "mmap"))

    for asset in config.get("assets", []):
        resources.append(build_resource_entry(asset, "assets", "littlefs"))

    return {
        "format_version": int(config.get("format_version", DEFAULT_FORMAT_VERSION)),
        "firmware_compat": config.get("firmware_compat", ""),
        "manifest_version": config.get("manifest_version", ""),
        "resources": resources,
    }


def build_metadata(config, ip, port, manifest):
    """生成总 OTA 元数据。"""
    return {
        "protocol_version": int(config.get("protocol_version", DEFAULT_PROTOCOL_VERSION)),
        "firmware": build_firmware_info(config, ip, port),
        "manifest": manifest,
    }


def write_generated_files(metadata, manifest):
    """写出启动后生成的调试文件。"""
    write_json(GENERATED_ROOT / "metadata.json", metadata)
    write_json(GENERATED_ROOT / "manifest.json", manifest)
    write_json(GENERATED_ROOT / "version.json", metadata["firmware"])


def prepare_server_state(config, ip, port):
    """根据当前配置和文件生成服务器运行态。"""
    ensure_server_layout()
    seed_demo_files()

    manifest = build_manifest(config)
    metadata = build_metadata(config, ip, port, manifest)
    write_generated_files(metadata, manifest)
    return metadata, manifest, metadata["firmware"]


class ReusableTCPServer(socketserver.TCPServer):
    """允许快速复用端口。"""

    allow_reuse_address = True


class OtaRequestHandler(http.server.SimpleHTTPRequestHandler):
    """支持动态元数据与静态文件的 OTA 测试服务器。"""

    metadata = {}
    manifest = {}
    firmware = {}

    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        super().end_headers()

    def send_json(self, payload):
        """发送 JSON 响应。"""
        body = json.dumps(payload, indent=2, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            print(f"Client disconnected early: {self.client_address[0]} - {self.path}")

    def do_GET(self):
        """处理 GET 请求。"""
        print(f"Request: {self.client_address[0]} - {self.path}")

        if self.path == METADATA_PATH:
            self.send_json(self.metadata)
            return

        if self.path == MANIFEST_ROUTE:
            self.send_json(self.manifest)
            return

        if self.path == LEGACY_VERSION_ROUTE:
            self.send_json(self.firmware)
            return

        try:
            super().do_GET()
        except (BrokenPipeError, ConnectionResetError):
            print(f"Client disconnected early: {self.client_address[0]} - {self.path}")


def print_server_summary(ip, port, firmware, manifest):
    """打印启动摘要。"""
    print("-" * 60)
    print(f"OTA Server running at http://{ip}:{port}/")
    print(f"Metadata URL : http://{ip}:{port}{METADATA_PATH}")
    print(f"Manifest URL : http://{ip}:{port}{MANIFEST_ROUTE}")
    print(f"Legacy URL   : http://{ip}:{port}{LEGACY_VERSION_ROUTE}")
    print(f"Firmware URL : {firmware['url']}")
    print(f"Config File  : {CONFIG_PATH}")
    print(f"Generated Dir: {GENERATED_ROOT}")
    print("Static resources:")
    for resource in manifest["resources"]:
        print(f"  - http://{ip}:{port}{resource_url_prefix(resource)}{resource['path']}")
    print("-" * 60)
    print("Press Ctrl+C to stop.")


def main():
    """启动 OTA 测试服务器。"""
    args = parse_args()
    config = load_config(args.config)
    metadata, manifest, firmware = prepare_server_state(config, args.ip, args.port)

    OtaRequestHandler.metadata = metadata
    OtaRequestHandler.manifest = manifest
    OtaRequestHandler.firmware = firmware
    handler = partial(OtaRequestHandler, directory=str(DATA_ROOT))

    print_server_summary(args.ip, args.port, firmware, manifest)

    with ReusableTCPServer(("", args.port), handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down.")


if __name__ == "__main__":
    main()
