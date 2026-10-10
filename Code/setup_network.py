"""Configure local Wi-Fi without putting passwords in shell history or chat."""
import getpass
import ipaddress
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def main():
    config_path = ROOT.parent / "LocalMQTT" / "data" / "connection.json"
    config = json.loads(config_path.read_text())
    ssid = input("2.4 GHz Wi-Fi network name: ")
    password = getpass.getpass("Wi-Fi password (hidden): ")
    if not 1 <= len(ssid.encode()) <= 32 or len(password.encode()) > 63:
        raise SystemExit("SSID must be 1–32 bytes and password at most 63 bytes.")
    host = input(f'Computer LAN IP [{config["broker_host"]}]: ').strip() or config["broker_host"]
    ipaddress.IPv4Address(host)
    values = {"TABLET_WIFI_SSID": ssid, "TABLET_WIFI_PASSWORD": password,
              "TABLET_MQTT_URI": f'mqtt://{host}:{config["mqtt_port"]}',
              "TABLET_MQTT_USER": config.get("device_user", ""),
              "TABLET_MQTT_PASSWORD": config.get("device_password", "")}
    # Encode C string literals with octal bytes so arbitrary UTF-8 is preserved.
    def literal(text):
        return '"' + ''.join(f'\\{byte:03o}' for byte in text.encode()) + '"'
    output = '#pragma once\n// Local secrets: do not commit this file.\n'
    output += ''.join(f'#define {key} {literal(value)}\n' for key, value in values.items())
    (ROOT / "main" / "network_config.h").write_text(output)
    # A first build without the optional header cannot track it as a dependency.
    # Force this source to rebuild when configuration is created afterward.
    (ROOT / "main" / "network.c").touch()
    print("Saved main/network_config.h. Now build and flash from your ESP-IDF shell.")


if __name__ == "__main__":
    main()
