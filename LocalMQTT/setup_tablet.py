"""Keep existing Wi-Fi settings and import this server's MQTT connection."""
import argparse
import json
from pathlib import Path
import re
from setup_lan import provision

ROOT = Path(__file__).resolve().parent

def configure(host):
    config = provision(host)
    header = ROOT.parent / "Code" / "main" / "network_config.h"
    if not header.exists():
        raise SystemExit("Run Code/setup_network.py to enter Wi-Fi settings first.")
    source = header.read_text()
    values = {"TABLET_MQTT_URI": f'mqtt://{host}:{config["mqtt_port"]}',
              "TABLET_MQTT_USER": config.get("device_user", ""),
              "TABLET_MQTT_PASSWORD": config.get("device_password", "")}
    for key, value in values.items():
        literal = '"' + ''.join(f'\\{byte:03o}' for byte in value.encode()) + '"'
        source, count = re.subn(r'^#define ' + key + r' .*$',
                                 lambda match: f'#define {key} {literal}', source, flags=re.M)
        if count != 1:
            raise SystemExit("Incomplete network header; run Code/setup_network.py.")
    header.write_text(source)
    (header.parent / "network.c").touch()
    print("MQTT settings updated; existing Wi-Fi settings preserved.")
    print(f"Phone/computer page: http://{host}:8080")
    print("Page login is in data/connection.json." if config.get("authentication_enabled", True)
          else "No webpage or MQTT login is required.")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    configure(parser.parse_args().host)
