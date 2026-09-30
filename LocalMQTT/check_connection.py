"""Verify the configured LAN listener with tablet credentials; no task edits."""
import asyncio
import json
from pathlib import Path
from urllib.parse import quote
from amqtt.client import MQTTClient


async def main():
    config = json.loads((Path(__file__).parent / "data/connection.json").read_text())
    client = MQTTClient(client_id="notepad-connection-check", config={"auto_reconnect": False})
    uri = (f'mqtt://{quote(config["device_user"], safe="")}:'
           f'{quote(config["device_password"], safe="")}@'
           f'{config["broker_host"]}:{config["mqtt_port"]}/')
    try:
        await asyncio.wait_for(client.connect(uri), 5)
        await client.subscribe([("notepad/v1/devices/tablet-001/state", 1)])
        message = await asyncio.wait_for(client.deliver_message(), 5)
        state = json.loads(message.data)
        print(f'Authenticated LAN subscription OK: revision {state["revision"]}, {len(state["items"])} tasks')
    finally:
        await client.disconnect()


if __name__ == "__main__":
    asyncio.run(main())
