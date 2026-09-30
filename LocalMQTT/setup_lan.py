"""Create local MQTT credentials without printing secrets. Run once before --lan."""
import argparse
import ipaddress
import json
from pathlib import Path
import secrets
from pwdlib import PasswordHash

ROOT = Path(__file__).resolve().parent


def provision(host, directory=ROOT / "data"):
    ipaddress.IPv4Address(host)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "connection.json"
    if path.exists():
        config = json.loads(path.read_text())
        config["broker_host"] = host
    else:
        config = {"broker_host": host, "mqtt_port": 1883,
                  "service_user": "notepad-service", "service_password": secrets.token_urlsafe(24),
                  "device_user": "tablet-001", "device_password": secrets.token_urlsafe(24)}
    hasher = PasswordHash.recommended()
    password_file = directory / "passwords.txt"
    password_file.write_text("\n".join(f'{config[role+"_user"]}:{hasher.hash(config[role+"_password"])}'
                                        for role in ("service", "device")) + "\n")
    config["password_file"] = str(password_file.resolve())
    path.write_text(json.dumps(config, indent=2) + "\n")
    print(f"LAN credentials saved in {path}. Passwords were not printed.")
    return config


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="This computer's LAN IPv4 address")
    provision(parser.parse_args().host)
