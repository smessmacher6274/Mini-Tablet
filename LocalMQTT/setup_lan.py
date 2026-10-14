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
    if not config.get("authentication_enabled", True):
        for key in list(config):
            if key.endswith("_user") or key.endswith("_password") or key == "password_file":
                del config[key]
        path.write_text(json.dumps(config, indent=2) + "\n")
        print(f"LAN configuration saved in {path}; authentication disabled.")
        return config
    hasher = PasswordHash.recommended()
    config.setdefault("web_user", "owner")
    config.setdefault("web_password", secrets.token_urlsafe(18))
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
    config = provision(parser.parse_args().host)
    print(f'Phone/computer page: http://{config["broker_host"]}:8080')
    if config.get("authentication_enabled", True):
        print(f'Page login: {config["web_user"]} / {config["web_password"]}')
    else:
        print("No webpage or MQTT login is required.")
