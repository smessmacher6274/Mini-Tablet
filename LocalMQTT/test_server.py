import asyncio
import json
from pathlib import Path
import socket
import tempfile
import unittest
import uuid

from aiohttp.test_utils import TestClient, TestServer
from amqtt.client import MQTTClient
from server import make_app, TOPIC
from server import PRESENCE
from setup_lan import provision
from urllib.parse import quote


class ServerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.database = Path(self.temp.name) / "tasks.db"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.mqtt_port = sock.getsockname()[1]
        await self.start()

    async def start(self):
        self.client = TestClient(TestServer(make_app(self.database, self.mqtt_port)))
        await self.client.start_server()

    async def asyncTearDown(self):
        await self.client.close()
        self.temp.cleanup()

    async def state(self):
        response = await self.client.get("/api/state")
        self.assertEqual(response.status, 200)
        return await response.json()

    async def add(self, title="Buy milk", request_id=None):
        response = await self.client.post("/api/items", json={"title": title, "request_id": request_id or str(uuid.uuid4())})
        self.assertEqual(response.status, 200, await response.text())
        return await response.json()

    async def published(self, revision):
        for _ in range(100):
            state = await self.state()
            if state["transport"]["published_revision"] == revision:
                return
            await asyncio.sleep(.05)
        self.fail("Snapshot was not published")

    async def retained(self):
        subscriber = MQTTClient(client_id="test-" + uuid.uuid4().hex, config={"auto_reconnect": False})
        try:
            await subscriber.connect(f"mqtt://127.0.0.1:{self.mqtt_port}/")
            await subscriber.subscribe([(TOPIC, 1)])
            message = await asyncio.wait_for(subscriber.deliver_message(), 5)
            return json.loads(message.data)
        finally:
            await subscriber.disconnect()

    async def test_crud_duplicate_conflict_and_retained_delivery(self):
        request_id = str(uuid.uuid4())
        first = await self.add(request_id=request_id)
        again = await self.add(request_id=request_id)
        self.assertEqual(first, again)
        item = first["items"][0]
        response = await self.client.patch(f'/api/items/{item["id"]}', json={"completed": True, "base_revision": item["revision"]})
        self.assertEqual(response.status, 200)
        changed = await response.json()
        current = changed["items"][0]
        self.assertTrue(current["completed"])
        stale = await self.client.patch(f'/api/items/{item["id"]}', json={"title": "Stale", "base_revision": item["revision"]})
        self.assertEqual(stale.status, 409)
        await self.published(changed["revision"])
        self.assertEqual(await self.retained(), changed)
        response = await self.client.patch(f'/api/items/{item["id"]}', json={"title": "Buy bread", "base_revision": current["revision"]})
        edited = await response.json()
        current = edited["items"][0]
        self.assertEqual(current["title"], "Buy bread")
        response = await self.client.delete(f'/api/items/{item["id"]}', json={"base_revision": current["revision"]})
        self.assertEqual(response.status, 200)
        deleted = await response.json()
        self.assertEqual(deleted["items"], [])
        await self.published(deleted["revision"])
        self.assertEqual((await self.retained())["items"], [])
        # Retrying the original add must not resurrect a deleted item.
        self.assertEqual((await self.add(request_id=request_id))["items"], [])

    async def test_restart_restores_database_and_retained_snapshot(self):
        state = await self.add("Survive a restart")
        await self.published(state["revision"])
        await self.client.close()
        await self.start()
        await self.published(state["revision"])
        self.assertEqual(await self.retained(), state)

    async def test_input_limits_and_origin_protection(self):
        for title in ["", "  ", "x" * 121, "\n", "line\nbreak", "🙂" * 31]:
            response = await self.client.post("/api/items", json={"title": title, "request_id": str(uuid.uuid4())})
            self.assertEqual(response.status, 400)
        response = await self.client.post("/api/items", json=[1,2])
        self.assertEqual(response.status, 400)
        response = await self.client.post("/api/items", json={"title":"No", "request_id":str(uuid.uuid4())}, headers={"Origin":"https://example.com"})
        self.assertEqual(response.status, 403)
        self.assertEqual((await self.state())["items"], [])
        for n in range(50):
            await self.add(f"Task {n}")
        response = await self.client.post("/api/items", json={"title":"Full", "request_id":str(uuid.uuid4())})
        self.assertEqual(response.status, 400)
        self.assertEqual(len((await self.state())["items"]), 50)


class LanTests(unittest.IsolatedAsyncioTestCase):
    async def test_credentials_snapshot_presence_and_acl(self):
        with tempfile.TemporaryDirectory() as directory:
            config = provision("127.0.0.1", Path(directory))
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0)); port = sock.getsockname()[1]
            client = TestClient(TestServer(make_app(Path(directory) / "tasks.db", port, config)))
            await client.start_server()
            device = MQTTClient(client_id="test-tablet", config={"auto_reconnect": False})
            denied = MQTTClient(client_id="test-denied", config={"auto_reconnect": False})
            try:
                with self.assertRaises(Exception):
                    await asyncio.wait_for(denied.connect(f"mqtt://tablet-001:wrong@127.0.0.1:{port}/"), 3)
                await device.connect(f'mqtt://{config["device_user"]}:{quote(config["device_password"])}@127.0.0.1:{port}/')
                granted = await device.subscribe([(TOPIC, 1)])
                self.assertEqual(granted, [1])
                packet = await asyncio.wait_for(device.deliver_message(), 5)
                self.assertEqual(json.loads(packet.data)["items"], [])
                # Other namespaces and writing server state are not device permissions.
                self.assertEqual(await device.subscribe([("unrelated/#", 1)]), [128])
                await device.publish(PRESENCE, b'{"online":true,"revision":0}', qos=1, retain=True)
                for _ in range(60):
                    state = await (await client.get("/api/state")).json()
                    if state["transport"]["device_connected"]:
                        break
                    await asyncio.sleep(.05)
                self.assertTrue(state["transport"]["device_connected"])
                self.assertEqual(state["transport"]["device_revision"], 0)
                await device.publish(PRESENCE, b'{"online":false}', qos=1, retain=True)
                await asyncio.sleep(.1)
                state = await (await client.get("/api/state")).json()
                self.assertFalse(state["transport"]["device_connected"])
            finally:
                await device.disconnect()
                await client.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
