import http.server
import socketserver
import json
import os
import socket
import sys

# Configuration
PORT = 8000
UPDATE_VERSION = "1.0.5"
UPDATE_FILENAME = "firmware.bin"
UPDATE_SIZE = 4 * 1024 * 1024  # 4MB dummy file
FIRMWARE_DIR = "firmware"
IS_MANDATORY = False # Set to True to test mandatory update check logic on device

def get_ip():
    import sys
    if len(sys.argv) > 1:
        return sys.argv[1]
    
    # 强制返回和 main.c 一致的 IP，避免获取到虚拟网卡 IP (如 26.26.26.1)
    return "192.168.3.17"

class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Access-Control-Allow-Origin', '*')
        super().end_headers()

    def do_GET(self):
        print(f"Request: {self.client_address[0]} - {self.path}")
        return super().do_GET()

def create_files(ip):
    # Ensure firmware directory exists
    if not os.path.exists(FIRMWARE_DIR):
        os.makedirs(FIRMWARE_DIR)
        
    fw_path = os.path.join(FIRMWARE_DIR, UPDATE_FILENAME)
    
    # Create dummy firmware if not exists
    if not os.path.exists(fw_path):
        print(f"Creating dummy firmware file: {fw_path}")
        with open(fw_path, "wb") as f:
            # Create a pattern so we can verify if needed, but random is fine
            f.write(os.urandom(UPDATE_SIZE))
    
    # Create version.json in root (so it's easily accessible)
    # The URL points to the file inside the firmware directory
    url = f"http://{ip}:{PORT}/{FIRMWARE_DIR}/{UPDATE_FILENAME}"
    data = {
        "version": UPDATE_VERSION,
        "url": url,
        "description": f"New firmware version {UPDATE_VERSION}, test release.",
        "mandatory": IS_MANDATORY,
        "size": os.path.getsize(fw_path)
    }
    
    with open("version.json", "w") as f:
        json.dump(data, f, indent=4)
    
    print(f"Created version.json pointing to {url}")

if __name__ == "__main__":
    # Change to the directory of this script to serve files from here
    os.chdir(os.path.dirname(os.path.abspath(__file__)))
    
    ip = get_ip()
    create_files(ip)
    
    print("-" * 60)
    print(f"OTA Server running at http://{ip}:{PORT}/")
    print(f"Update URL for ESP32: http://{ip}:{PORT}/version.json")
    print("-" * 60)
    print("Press Ctrl+C to stop.")
    
    with socketserver.TCPServer(("", PORT), Handler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down.")
